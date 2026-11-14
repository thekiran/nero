/* Wi-Fi command surface and terminal dashboards. */

#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "nero/operation_ui.h"
#include "nero/table.h"
#include "nero/terminal.h"
#include "nero/wifi.h"
#include "nero/wifi_capture.h"
#include "nero/wifi_platform.h"
#include "nero/wifi_nl80211.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif

#define WIFI_IDLE_SECONDS 60

static nero_wifi_session *wifi_session;
static int wifi_atexit_registered;

static uint64_t anonymization_salt(void);
static void anonymize_mac_address(const uint8_t input[NERO_WIFI_MAC_LEN],
                                  uint8_t output[NERO_WIFI_MAC_LEN]);
static void sleep_milliseconds(int milliseconds);
static void print_nl80211_status(const char *interface);
static void print_nl80211_capabilities(const char *interface);

static void wifi_shutdown(void)
{
    if (wifi_session != NULL) {
        nero_wifi_session_destroy(wifi_session);
        wifi_session = NULL;
    }
}

static nero_wifi_session *wifi_get_session(void)
{
    if (wifi_session == NULL) {
        wifi_session = nero_wifi_session_create();
        if (wifi_session != NULL && !wifi_atexit_registered) {
            atexit(wifi_shutdown);
            wifi_atexit_registered = 1;
        }
    }
    return wifi_session;
}

static void print_wifi_help(void)
{
    puts("\nNERO WIRELESS MONITORING\n"
         "  wifi status                         Show adapter, capabilities and capture state\n"
         "  wifi capabilities                   Show runtime capability matrix and limits\n"
         "  wifi interfaces                     List Wi-Fi interfaces\n"
         "  wifi scan [--band B] [--channel N] [--live] [INTERFACE]\n"
         "                                      Active survey (never joins a network)\n"
         "  wifi aps [--channel N]              Show access points from passive data\n"
         "  wifi clients [--channel N] [--bssid MAC] [--live] [--anonymize]\n"
         "                                      Show clients grouped by access point\n"
         "  wifi signal [--live]                Signal diagnostics (Ctrl+C stops live mode)\n"
         "  wifi inspect <MAC|BSSID>            Inspect a client or access point\n"
         "  wifi monitor [--link] [INTERFACE]   Live survey (interactive table on a TTY)\n"
         "  wifi monitor --hop [--dwell MS] [--bands 2.4,5,6]\n"
         "                                      Opt-in channel hopping (Linux, root, consented)\n"
         "  wifi monitor start [--link] [I]     Start a background passive session\n"
         "  wifi monitor stop                   Stop the background session\n"
         "  wifi monitor status                 Show session counters\n"
         "  wifi monitor enable [INTERFACE]     Switch the interface to monitor mode (Linux)\n"
         "  wifi monitor disable [INTERFACE]    Restore the previous interface type (Linux)\n"
         "  wifi analyze <PCAP_FILE>            Analyze a pcap/pcapng capture offline\n"
         "  wifi export [--anonymize] [--channel N] [FILE]\n"
         "                                      Export observed data to CSV\n"
         "  wifi clear                          Stop capture and discard data in memory\n"
         "\nCapture is passive and read-only. No injection, deauthentication or decryption.\n"
         "MAC addresses are never persisted unless you explicitly export them.");
}

static const char *display_ssid(const nero_wifi_ap *ap, char *buffer, size_t buffer_len)
{
    if (ap->redacted && ap->ssid_len == 0) {
        snprintf(buffer, buffer_len, "<redacted>");
    } else if (ap->hidden || ap->ssid_len == 0) {
        snprintf(buffer, buffer_len, "<hidden>");
    } else {
        snprintf(buffer, buffer_len, "%s", ap->ssid);
    }
    return buffer;
}

static const char *display_bssid(const nero_wifi_ap *ap, char *buffer, size_t buffer_len)
{
    if (!ap->has_bssid) {
        snprintf(buffer, buffer_len, "<redacted>");
    } else {
        nero_wifi_format_mac(ap->bssid, buffer, buffer_len);
    }
    return buffer;
}

static const char *display_client_bssid(const nero_wifi_client *client, char *buffer,
                                        size_t buffer_len)
{
    if (!client->has_bssid) {
        snprintf(buffer, buffer_len, "-");
    } else {
        nero_wifi_format_mac(client->bssid, buffer, buffer_len);
    }
    return buffer;
}

static void format_age(const nero_wifi_client *client, int64_t now, char *buffer, size_t buffer_len)
{
    int64_t seconds = (now - client->last_seen_us) / 1000000LL;
    if (seconds < 0)
        seconds = 0;
    if (seconds < 60)
        snprintf(buffer, buffer_len, "%llds", (long long)seconds);
    else if (seconds < 3600)
        snprintf(buffer, buffer_len, "%lldm", (long long)(seconds / 60));
    else
        snprintf(buffer, buffer_len, "%lldh", (long long)(seconds / 3600));
}

/* ------------------------------------------------------------------------- *
 *  Wi-Fi presentation layer.
 *
 *  The responsive grid itself lives in the shared, capability-gated table
 *  engine (nero/table.h). This file only supplies the Wi-Fi column specs, row
 *  builders and semantic colours (signal strength, security posture, radio
 *  band); colours encode meaning only and never invent data. On NO_COLOR,
 *  non-ANSI or non-UTF-8 terminals the engine falls back to a plain ASCII grid
 *  so the output stays copy-pasteable and scriptable.
 * ------------------------------------------------------------------------- */

#define WIFI_RESET NERO_C_RESET
#define WIFI_DIM   NERO_C_DIM
#define WIFI_HEAD  NERO_C_HEAD
#define WIFI_GREEN NERO_C_GREEN
#define WIFI_YEL   NERO_C_YELLOW
#define WIFI_RED   NERO_C_RED
#define WIFI_CYAN  NERO_C_CYAN
#define WIFI_MAG   NERO_C_MAGENTA

static int wifi_color(void) { return terminal_color_enabled(); }
static int wifi_unicode(void) { return terminal_unicode_enabled(); }

/* Returns the colour escape when colour is enabled, else an empty string. */
static const char *wc(const char *code) { return wifi_color() ? code : ""; }
static const char *wc_off(void) { return wifi_color() ? WIFI_RESET : ""; }

/* Usable grid width: the shared single source, capped so an ultrawide terminal
 * does not produce a single 200-cell column. */
static int wifi_target_width(void)
{
    int width = nero_terminal_width();
    if (width > NERO_TABLE_DEFAULT_MAX)
        width = NERO_TABLE_DEFAULT_MAX;
    return width;
}

/* Fills render options from the live capability gates for every Wi-Fi table. */
static void wifi_table_opts(nero_table_opts *opts) { nero_table_opts_default(opts); }

static const char *band_color(nero_wifi_band band)
{
    switch (band) {
    case NERO_WIFI_BAND_2GHZ: return WIFI_YEL;
    case NERO_WIFI_BAND_5GHZ: return WIFI_CYAN;
    case NERO_WIFI_BAND_6GHZ: return WIFI_MAG;
    default: return WIFI_DIM;
    }
}

static const char *security_color(nero_wifi_security security)
{
    switch (security) {
    case NERO_WIFI_SEC_OPEN:
    case NERO_WIFI_SEC_WEP: return WIFI_RED;
    case NERO_WIFI_SEC_WPA: return WIFI_YEL;
    case NERO_WIFI_SEC_WPA2:
    case NERO_WIFI_SEC_WPA2_WPA3:
    case NERO_WIFI_SEC_WPA3: return WIFI_GREEN;
    case NERO_WIFI_SEC_OWE: return WIFI_CYAN;
    default: return WIFI_DIM;
    }
}

static const nero_table_col ap_cols[] = {
    {"SIGNAL", 9, 9, 0, 0, 0, 0, NERO_TABLE_SIGNAL},
    {"SSID", 10, 22, 1, 0, 0, 24, NERO_TABLE_TEXT},
    {"BSSID", 17, 17, 0, 0, 4, 0, NERO_TABLE_TEXT},
    {"CH", 3, 3, 0, 1, 3, 0, NERO_TABLE_TEXT},
    {"BAND", 5, 7, 0, 0, 2, 0, NERO_TABLE_TEXT},
    {"SECURITY", 6, 10, 0, 0, 5, 0, NERO_TABLE_TEXT},
    {"CLIENTS", 7, 7, 0, 1, 2, 0, NERO_TABLE_TEXT},
    {"LAST SEEN", 19, 19, 0, 0, 1, 0, NERO_TABLE_TEXT},
};

static void ap_row_build(int index, void *ctx, nero_table_cell *out)
{
    const nero_wifi_ap *ap = &((const nero_wifi_ap *)ctx)[index];
    char ssid[64];
    char bssid[32];
    char channel[8];
    char clients[8];
    char last[32];
    int hidden = ap->hidden || ap->redacted || ap->ssid_len == 0;

    if (ap->has_channel)
        snprintf(channel, sizeof(channel), "%d", ap->channel);
    else
        snprintf(channel, sizeof(channel), "-");
    snprintf(clients, sizeof(clients), "%d", ap->client_count);
    nero_wifi_format_time(ap->last_seen_us, last, sizeof(last));

    out[0].has_rssi = ap->has_rssi;
    out[0].rssi = ap->rssi;
    snprintf(out[1].text, sizeof(out[1].text), "%s", display_ssid(ap, ssid, sizeof(ssid)));
    out[1].color = hidden ? WIFI_DIM : NULL;
    snprintf(out[2].text, sizeof(out[2].text), "%s", display_bssid(ap, bssid, sizeof(bssid)));
    snprintf(out[3].text, sizeof(out[3].text), "%s", channel);
    snprintf(out[4].text, sizeof(out[4].text), "%s",
             ap->band == NERO_WIFI_BAND_UNKNOWN ? "-" : nero_wifi_band_name(ap->band));
    out[4].color = band_color(ap->band);
    snprintf(out[5].text, sizeof(out[5].text), "%s", nero_wifi_security_name(ap->security));
    out[5].color = security_color(ap->security);
    snprintf(out[6].text, sizeof(out[6].text), "%s", clients);
    snprintf(out[7].text, sizeof(out[7].text), "%s", last);
    out[7].color = WIFI_DIM;
}

static void print_ap_table(const nero_wifi_ap *aps, int count)
{
    printf("\n%sACCESS POINTS%s  %s(%d observed)%s\n", wc(WIFI_HEAD), wc_off(), wc(WIFI_DIM), count,
           wc_off());
    if (count == 0) {
        printf("  %s(no access points in the current data set)%s\n", wc(WIFI_DIM), wc_off());
        return;
    }
    nero_table_opts opts;
    wifi_table_opts(&opts);
    nero_table_draw(stdout, &opts, ap_cols, (int)(sizeof(ap_cols) / sizeof(ap_cols[0])), count,
                    ap_row_build, (void *)aps);
}

static const char *confidence_color(nero_wifi_confidence confidence)
{
    switch (confidence) {
    case NERO_WIFI_CONFIDENCE_ASSOCIATED: return WIFI_GREEN;
    case NERO_WIFI_CONFIDENCE_LIKELY: return WIFI_YEL;
    default: return WIFI_DIM;
    }
}

static const nero_table_col client_cols[] = {
    {"DEVICE MAC", 17, 17, 0, 0, 0, 0, NERO_TABLE_TEXT},
    {"SIGNAL", 9, 9, 0, 0, 0, 0, NERO_TABLE_SIGNAL},
    {"STATUS", 8, 11, 0, 0, 4, 0, NERO_TABLE_TEXT},
    {"AGE", 4, 6, 0, 1, 2, 0, NERO_TABLE_TEXT},
    {"NOTES", 8, 18, 1, 0, 1, 12, NERO_TABLE_TEXT},
};

typedef struct {
    const nero_wifi_client *clients;
    int anonymize;
    int64_t now;
} client_table_ctx;

static void client_row_build(int index, void *vctx, nero_table_cell *out)
{
    const client_table_ctx *ctx = vctx;
    const nero_wifi_client *client = &ctx->clients[index];
    char mac[32];
    char age[24];
    char notes[64] = "";

    nero_wifi_format_mac(client->mac, mac, sizeof(mac));
    if (ctx->anonymize) {
        uint8_t clean[NERO_WIFI_MAC_LEN];
        anonymize_mac_address(client->mac, clean);
        nero_wifi_format_mac(clean, mac, sizeof(mac));
    }
    format_age(client, ctx->now, age, sizeof(age));
    if (client->state == NERO_WIFI_STATE_DISASSOCIATED)
        strncat(notes, "LEFT ", sizeof(notes) - strlen(notes) - 1);
    if (client->randomized)
        strncat(notes, "RANDOM-MAC ", sizeof(notes) - strlen(notes) - 1);
    if ((ctx->now - client->last_seen_us) > (int64_t)WIFI_IDLE_SECONDS * 1000000LL)
        strncat(notes, "IDLE ", sizeof(notes) - strlen(notes) - 1);

    snprintf(out[0].text, sizeof(out[0].text), "%s", mac);
    out[0].color = client->randomized ? WIFI_DIM : NULL;
    out[1].has_rssi = client->has_rssi;
    out[1].rssi = client->rssi;
    snprintf(out[2].text, sizeof(out[2].text), "%s",
             nero_wifi_confidence_name(client->confidence));
    out[2].color = confidence_color(client->confidence);
    snprintf(out[3].text, sizeof(out[3].text), "%s", age);
    out[3].color = WIFI_DIM;
    snprintf(out[4].text, sizeof(out[4].text), "%s", notes);
    out[4].color = notes[0] ? WIFI_YEL : NULL;
}

static void client_group_label(const nero_wifi_client *group, int anonymize)
{
    char bssid[32];
    char label[128];
    char line[192];
    char fitted[192];
    if (group->has_bssid) {
        nero_wifi_format_mac(group->bssid, bssid, sizeof(bssid));
        if (anonymize) {
            uint8_t clean[NERO_WIFI_MAC_LEN];
            anonymize_mac_address(group->bssid, clean);
            nero_wifi_format_mac(clean, bssid, sizeof(bssid));
        }
    } else {
        snprintf(bssid, sizeof(bssid), "-");
    }
    if (group->ssid[0] != '\0')
        snprintf(label, sizeof(label), "%s", group->ssid);
    else
        snprintf(label, sizeof(label), "<hidden/unknown>");
    snprintf(line, sizeof(line), "%s (%s)", label, bssid);
    int room = wifi_target_width() - 4; /* "  > " prefix */
    nero_fit(line, room, !wifi_unicode(), fitted, sizeof(fitted));
    printf("\n  %s%s%s %s%s%s\n", wc(WIFI_CYAN), wifi_unicode() ? "▸" : ">", wc_off(), wc(WIFI_HEAD),
           fitted, wc_off());
}

static int client_matches(const nero_wifi_client *client, const uint8_t *only_bssid)
{
    if (only_bssid == NULL)
        return 1;
    return client->has_bssid && memcmp(client->bssid, only_bssid, NERO_WIFI_MAC_LEN) == 0;
}

static void print_client_rows(const nero_wifi_client *clients, int count, int anonymize,
                              const uint8_t *only_bssid)
{
    int64_t now = nero_wifi_now_us();
    int groups = 0;
    int i = 0;

    while (i < count) {
        if (!client_matches(&clients[i], only_bssid)) {
            ++i;
            continue;
        }
        int start = i++;
        while (i < count && client_matches(&clients[i], only_bssid) &&
               memcmp(clients[i].bssid, clients[start].bssid, NERO_WIFI_MAC_LEN) == 0 &&
               strcmp(clients[i].ssid, clients[start].ssid) == 0) {
            ++i;
        }
        client_group_label(&clients[start], anonymize);
        client_table_ctx ctx = {clients + start, anonymize, now};
        nero_table_opts opts;
        wifi_table_opts(&opts);
        nero_table_draw(stdout, &opts, client_cols,
                        (int)(sizeof(client_cols) / sizeof(client_cols[0])), i - start,
                        client_row_build, &ctx);
        ++groups;
    }
    if (groups == 0)
        printf("  %s(no client devices in the current data set)%s\n", wc(WIFI_DIM), wc_off());
}

static void print_client_table(const nero_wifi_client *clients, int count, int anonymize)
{
    printf("\n%sCLIENT DEVICES%s  %s(grouped by access point)%s\n", wc(WIFI_HEAD), wc_off(),
           wc(WIFI_DIM), wc_off());
    print_client_rows(clients, count, anonymize, NULL);
}

static void print_channel_summary(const nero_wifi_ap *aps, int count)
{
    int channels[256] = {0};
    int any = 0;
    for (int i = 0; i < count; ++i) {
        if (aps[i].has_channel && aps[i].channel >= 0 && aps[i].channel < 256) {
            ++channels[aps[i].channel];
            any = 1;
        }
    }
    if (!any)
        return;
    int max = 1;
    for (int i = 0; i < 256; ++i)
        if (channels[i] > max)
            max = channels[i];
    printf("\n%sCHANNEL USAGE%s\n", wc(WIFI_HEAD), wc_off());
    const char *block = wifi_unicode() ? "█" : "#";
    int bar_room = wifi_target_width() - 26; /* "  chNNN " prefix + count suffix */
    if (bar_room < 8)
        bar_room = 8;
    if (bar_room > 40)
        bar_room = 40;
    for (int i = 0; i < 256; ++i) {
        if (channels[i] <= 0)
            continue;
        int bars = (channels[i] * bar_room + max - 1) / max;
        if (bars < 1)
            bars = 1;
        const char *color = i <= 14 ? WIFI_YEL : (i < 100 ? WIFI_CYAN : WIFI_MAG);
        printf("  %sch%3d%s %s", wc(WIFI_DIM), i, wc_off(), wc(color));
        for (int k = 0; k < bars; ++k)
            fputs(block, stdout);
        printf("%s %s%d AP%s%s\n", wc_off(), wc(WIFI_DIM), channels[i], channels[i] == 1 ? "" : "s",
               wc_off());
    }
}

/* A capture-statistics key/value row, rendered through the shared table. */
typedef struct {
    const char *label;
    char value[96];
    const char *color;
} wifi_stat_row;

static const nero_table_col stats_cols[] = {
    {"FIELD", 10, 14, 0, 0, 0, 0, NERO_TABLE_TEXT},
    {"VALUE", 16, 60, 1, 0, 0, 48, NERO_TABLE_TEXT},
};

static void stats_row_build(int index, void *ctx, nero_table_cell *out)
{
    const wifi_stat_row *rows = ctx;
    snprintf(out[0].text, sizeof(out[0].text), "%s", rows[index].label);
    out[0].color = WIFI_DIM;
    snprintf(out[1].text, sizeof(out[1].text), "%s", rows[index].value);
    out[1].color = rows[index].color;
}

static void print_stats_summary(const nero_wifi_stats *stats)
{
    wifi_stat_row rows[11];
    int n = 0;

    rows[n].label = "Source";
    rows[n].color = NULL;
    /* stats->source is a long path buffer; it is deliberately clipped to the
     * value cell (the table also fits it to the column). The explicit precision
     * tells the compiler the write is bounded. */
    snprintf(rows[n].value, sizeof(rows[n].value), "%.*s", (int)sizeof(rows[n].value) - 1,
             stats->source[0] != '\0' ? stats->source : "(no capture source)");
    ++n;
    rows[n].label = "Link type";
    rows[n].color = NULL;
    snprintf(rows[n].value, sizeof(rows[n].value), "%d", stats->linktype);
    ++n;
    rows[n].label = "Packets";
    rows[n].color = WIFI_CYAN;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->packets);
    ++n;
    rows[n].label = "Frames";
    rows[n].color = NULL;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu  (mgmt %llu, data %llu, ctrl %llu)",
             (unsigned long long)stats->frames, (unsigned long long)stats->management,
             (unsigned long long)stats->data, (unsigned long long)stats->control);
    ++n;
    rows[n].label = "Bytes";
    rows[n].color = NULL;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->bytes);
    ++n;
    rows[n].label = "Malformed";
    rows[n].color = stats->malformed ? WIFI_YEL : WIFI_DIM;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->malformed);
    ++n;
    rows[n].label = "Duplicates";
    rows[n].color = WIFI_DIM;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->duplicates);
    ++n;
    rows[n].label = "Non-802.11";
    rows[n].color = WIFI_DIM;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->non_wifi);
    ++n;
    rows[n].label = "Dropped";
    rows[n].color = stats->dropped ? WIFI_YEL : WIFI_DIM;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu  (capture + table pressure)",
             (unsigned long long)stats->dropped);
    ++n;
    rows[n].label = "Evicted";
    rows[n].color = WIFI_DIM;
    snprintf(rows[n].value, sizeof(rows[n].value), "%llu", (unsigned long long)stats->evicted);
    ++n;

    printf("\n%sCAPTURE STATISTICS%s\n", wc(WIFI_HEAD), wc_off());
    nero_table_opts opts;
    wifi_table_opts(&opts);
    nero_table_draw(stdout, &opts, stats_cols, (int)(sizeof(stats_cols) / sizeof(stats_cols[0])), n,
                    stats_row_build, rows);
}

static int parse_number(const char *text, int minimum, int maximum, int *out)
{
    if (text == NULL || out == NULL)
        return -1;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum)
        return -1;
    *out = (int)value;
    return 0;
}

static int parse_band(const char *text, nero_wifi_band *out)
{
    if (text == NULL || out == NULL)
        return -1;
    if (strcmp(text, "2.4") == 0 || strcmp(text, "2.4ghz") == 0 || strcmp(text, "2g") == 0 ||
        strcmp(text, "2") == 0) {
        *out = NERO_WIFI_BAND_2GHZ;
        return 0;
    }
    if (strcmp(text, "5") == 0 || strcmp(text, "5g") == 0 || strcmp(text, "5ghz") == 0) {
        *out = NERO_WIFI_BAND_5GHZ;
        return 0;
    }
    if (strcmp(text, "6") == 0 || strcmp(text, "6g") == 0 || strcmp(text, "6ghz") == 0) {
        *out = NERO_WIFI_BAND_6GHZ;
        return 0;
    }
    return -1;
}

/* Channel-hopping band bitmask (see the opt-in hopping section). Defined here so
 * the always-compiled monitor argument parser can use wifi_parse_hop_bands even
 * in offline builds where the live survey is compiled out. */
#define WIFI_HOP_2GHZ 1u
#define WIFI_HOP_5GHZ 2u
#define WIFI_HOP_6GHZ 4u

/* Parses a comma-separated band list ("2.4,5,6") into a hop bitmask. Returns 0
 * when nothing valid was found so the caller can reject the argument. */
static unsigned wifi_parse_hop_bands(const char *text)
{
    unsigned mask = 0;
    const char *p = text;
    while (p != NULL && *p != '\0') {
        while (*p == ',' || *p == ' ')
            ++p;
        if (*p == '\0')
            break;
        if (strncmp(p, "2.4", 3) == 0 || p[0] == '2')
            mask |= WIFI_HOP_2GHZ;
        else if (p[0] == '5')
            mask |= WIFI_HOP_5GHZ;
        else if (p[0] == '6')
            mask |= WIFI_HOP_6GHZ;
        while (*p != '\0' && *p != ',')
            ++p;
    }
    return mask;
}

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
static int filter_aps_by_radio(nero_wifi_ap *aps, int count, nero_wifi_band band, int channel)
{
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        if (channel >= 0 && (!aps[i].has_channel || aps[i].channel != channel))
            continue;
        if (band != NERO_WIFI_BAND_UNKNOWN && aps[i].band != band)
            continue;
        if (kept != i)
            aps[kept] = aps[i];
        ++kept;
    }
    return kept;
}

static const char *signal_quality_name(int rssi)
{
    if (rssi >= -50)
        return "excellent";
    if (rssi >= -60)
        return "good";
    if (rssi >= -70)
        return "fair";
    if (rssi >= -80)
        return "weak";
    return "very weak";
}
#endif

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
static const char *default_interface(void)
{
    static char name[NERO_WIFI_IFNAME_MAX];
    nero_wifi_platform_status status;
    char error[256];
    if (nero_wifi_platform_get_status(&status, error, sizeof(error)) == 0 &&
        status.interface[0] != '\0') {
        snprintf(name, sizeof(name), "%s", status.interface);
        return name;
    }
#if defined(__APPLE__)
    return "en0";
#elif defined(_WIN32)
    return "";
#else
    return "wlan0";
#endif
}
#endif

static cli_cmd_result cmd_wifi_status(void)
{
    nero_wifi_platform_status status;
    char error[256] = {0};
    if (nero_wifi_platform_get_status(&status, error, sizeof(error)) != 0) {
        printf("\nWi-Fi status unavailable: %s\n", error[0] != '\0' ? error : "unknown error");
        return CLI_CMD_CONTINUE;
    }
    printf("\nNERO WIRELESS STATUS\n"
           "  OS:            %s %s (%s)\n"
           "  Model:         %s\n"
           "  Chip:          %s\n"
           "  Interface:     %s\n"
           "  Driver:        %s\n"
           "  Powered:       %s\n"
           "  Service:       %s\n",
           status.os_name, status.os_version, status.architecture,
           status.model[0] != '\0' ? status.model : "unknown",
           status.chip[0] != '\0' ? status.chip : "unknown",
           status.interface[0] != '\0' ? status.interface : "(none detected)",
           status.driver[0] != '\0' ? status.driver : "unknown",
           status.wifi_powered ? "yes" : "no/unknown",
           status.service_active ? "yes" : "no/unknown");
    if (status.has_channel) {
        printf("  Channel:       %d (%s)\n", status.channel, nero_wifi_band_name(status.band));
    }
    if (status.has_rssi)
        printf("  RSSI:          %d dBm\n", status.rssi);
    printf("  Active scan:   %s\n",
           status.capabilities.active_scan_available ? "supported (CoreWLAN)" : "unsupported");
    printf("  Passive core:  %s\n", status.capabilities.corewlan_available
                                        ? "CoreWLAN present (metadata only)"
                                        : "CoreWLAN unavailable");

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    puts("  Capture:       unsupported in this offline build");
#else
    int backend = 0;
    int can_monitor = 0;
    char backend_name[64] = {0};
    char capture_error[256] = {0};
    const char *interface = status.interface[0] != '\0' ? status.interface : default_interface();
    if (interface[0] != '\0') {
        nero_wifi_capture_probe(interface, &backend, &can_monitor, backend_name,
                                sizeof(backend_name), capture_error, sizeof(capture_error));
        printf("  Capture:       %s (%s)\n", backend ? backend_name : "unavailable",
               backend ? "read-only libpcap" : capture_error);
        printf("  Monitor mode:  %s\n",
               backend && can_monitor
                   ? "libpcap reports supported (delivery must be verified with wifi monitor)"
                   : (backend ? "not supported by this backend/interface" : "unverified"));
    }
#endif
    print_nl80211_status(status.interface[0] != '\0' ? status.interface : NULL);
    nero_wifi_session *session = wifi_get_session();
    if (session != NULL) {
        nero_wifi_stats stats;
        char last_error[256] = {0};
        nero_wifi_session_snapshot_stats(session, &stats);
        printf("  Session:       %s\n", nero_wifi_session_running(session) ? "capturing" : "idle");
        if (stats.packets > 0 || stats.frames > 0) {
            printf("  Last data:     %llu packets, %llu frames\n",
                   (unsigned long long)stats.packets, (unsigned long long)stats.frames);
        }
        if (nero_wifi_session_last_error(session, last_error, sizeof(last_error)) > 0) {
            printf("  Last error:    %s\n", last_error);
        }
    }
    return CLI_CMD_CONTINUE;
}

static const char *active_scan_status(const nero_wifi_platform_status *status)
{
    if (status->capabilities.active_scan_available)
        return "supported (CoreWLAN active scan)";
#if defined(__APPLE__)
    return "unsupported (CoreWLAN unavailable)";
#elif defined(_WIN32)
    return "unsupported (Native Wi-Fi scan not implemented)";
#else
    return "unsupported (no OS-managed scan backend)";
#endif
}

static cli_cmd_result cmd_wifi_capabilities(void)
{
    nero_wifi_platform_status status;
    char error[256] = {0};
    if (nero_wifi_platform_get_status(&status, error, sizeof(error)) != 0) {
        printf("\nwifi capabilities unavailable: %s\n",
               error[0] != '\0' ? error : "unknown error");
        return CLI_CMD_CONTINUE;
    }
    nero_wifi_interface_info interfaces[16];
    int interface_count = 0;
    char list_error[256] = {0};
    if (nero_wifi_platform_list_interfaces(interfaces, 16, &interface_count, list_error,
                                           sizeof(list_error)) != 0) {
        interface_count = 0;
    }

    printf("\n%sNERO WIRELESS CAPABILITIES%s\n", wc(WIFI_HEAD), wc_off());
    printf("  Platform:         %s %s (%s)\n", status.os_name, status.os_version,
           status.architecture);
    printf("  Platform backend: %s\n",
           status.capabilities.backend[0] != '\0' ? status.capabilities.backend : "unknown");
    printf("  Wi-Fi interfaces: %d\n", interface_count);
    printf("  Active adapter:   %s\n",
           status.interface[0] != '\0' ? status.interface : "(none detected)");
    printf("  Driver:           %s\n",
           status.driver[0] != '\0' ? status.driver : "unknown");
    printf("  Active scan:      %s\n", active_scan_status(&status));
    printf("  Metadata core:    %s\n", status.capabilities.corewlan_available
                                            ? "CoreWLAN available"
                                            : "CoreWLAN unavailable");

    const char *probe_interface =
        status.interface[0] != '\0' ? status.interface
                                     : (interface_count > 0 ? interfaces[0].name : NULL);
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    puts("  Passive capture:  unavailable in this offline build");
    puts("  Monitor mode:     unavailable in this offline build");
    puts("  Delivery evidence: unavailable in this offline build");
#else
    int backend = 0;
    int can_monitor = 0;
    char backend_name[64] = {0};
    char capture_error[256] = {0};
    if (probe_interface != NULL && probe_interface[0] != '\0') {
        nero_wifi_capture_probe(probe_interface, &backend, &can_monitor, backend_name,
                                sizeof(backend_name), capture_error, sizeof(capture_error));
        printf("  Passive capture:  %s\n",
               backend ? backend_name
                       : (capture_error[0] != '\0' ? capture_error : "unavailable"));
        printf("  Monitor request:  %s\n",
               backend ? (can_monitor ? "supported by libpcap (delivery must be verified)"
                                      : "not supported for this interface/backend")
                       : "unavailable");
    } else {
        puts("  Passive capture:  no Wi-Fi interface to probe");
        puts("  Monitor request:  unavailable");
    }
    nero_wifi_session *session = wifi_get_session();
    if (session != NULL) {
        nero_wifi_stats stats;
        nero_wifi_session_snapshot_stats(session, &stats);
        if (stats.frames > 0) {
            printf("  Delivery evidence: verified (%llu 802.11 frame(s), link type %d, "
                   "monitor %s)\n",
                   (unsigned long long)stats.frames, stats.linktype,
                   stats.monitor_set ? "requested" : "not requested");
        } else {
            puts("  Delivery evidence: not verified (no 802.11 frames captured yet)");
        }
    }
#endif
    print_nl80211_capabilities(probe_interface);

    puts("\n  CAPABILITY MATRIX (runtime)");
    puts("    Interface discovery        supported");
    printf("    OS-managed active scan     %s\n", active_scan_status(&status));
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    puts("    Monitor mode request       unsupported in this build");
    puts("    Passive 802.11 capture     unsupported in this build");
#else
    puts("    Monitor mode request       driver/backend dependent (libpcap probe above)");
#if defined(__linux__)
    puts("    Kernel monitor control     nl80211 (requires root/CAP_NET_ADMIN)");
    puts("    Channel hopping (--hop)    nl80211 SET_CHANNEL (root, monitor mode, opt-in+consent)");
#else
    puts("    Kernel monitor control     unsupported on this platform (Linux nl80211 only)");
    puts("    Channel hopping (--hop)    unsupported on this platform (Linux nl80211 only)");
#endif
    puts("    Passive 802.11 capture     requires a monitor-capable driver and libpcap/Npcap");
    puts("    Default capture is read-only and stays on the adapter's current channel.");
#endif
    puts("    Client observation         requires passive frames; active scans cannot prove it");
    puts("    Signal diagnostics         RSSI/channel when exposed; no invented noise/SNR");
    puts("\n  LIMITATIONS");
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    puts("    Offline build: no interface is opened, scanned or changed.");
#elif defined(__APPLE__)
    puts("    macOS may redact SSID/BSSID without Location Services permission.");
    puts("    CoreWLAN scans are active. Integrated Apple Silicon adapters may accept");
    puts("    monitor mode but deliver zero frames; verify with `wifi monitor`.");
#elif defined(_WIN32)
    puts("    Passive capture requires Npcap with monitor-mode support.");
    puts("    Native Wi-Fi active scanning is not implemented in this backend.");
#else
    puts("    Monitor mode and regulatory channel availability depend on the driver/firmware.");
    puts("    NERO probes capabilities but never changes channel or regulatory settings.");
#endif
    return CLI_CMD_CONTINUE;
}

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
#define NERO_WIFI_SIGNAL_HISTORY 40

static void signal_history_add(int *history, size_t *count, int rssi)
{
    if (*count < NERO_WIFI_SIGNAL_HISTORY) {
        history[(*count)++] = rssi;
    } else {
        memmove(history, history + 1, (NERO_WIFI_SIGNAL_HISTORY - 1) * sizeof(history[0]));
        history[NERO_WIFI_SIGNAL_HISTORY - 1] = rssi;
    }
}

static void print_signal_snapshot(const nero_wifi_platform_status *status, const int *history,
                                  size_t history_count)
{
    printf("\n%sNERO WIRELESS SIGNAL%s\n", wc(WIFI_HEAD), wc_off());
    printf("  Interface:  %s\n",
           status->interface[0] != '\0' ? status->interface : "(none detected)");
    printf("  Adapter:    %s\n",
           status->wifi_powered ? "powered on" : "off or unknown");
    if (status->has_rssi) {
        const char *color = WIFI_DIM;
        (void)nero_signal_level(1, status->rssi, &color);
        printf("  RSSI:       %s%d dBm%s\n", wc(color), status->rssi, wc_off());
        printf("  Quality:    %s (rough estimate derived from RSSI)\n",
               signal_quality_name(status->rssi));
    } else {
        puts("  RSSI:       not exposed by this platform backend");
    }
    if (status->has_channel) {
        int frequency = nero_wifi_channel_to_frequency(status->channel, status->band);
        printf("  Channel:    %d (%s", status->channel, nero_wifi_band_name(status->band));
        if (frequency > 0)
            printf(", %d MHz", frequency);
        puts(")");
    } else {
        puts("  Channel:    not exposed by this platform backend");
    }
    puts("  Noise/SNR:  not exposed by this platform backend");
    puts("  Bitrate:    not exposed by this platform backend");

    if (history_count > 0) {
        static const char ascii_levels[] = " .:-=+*#%@";
        static const char *const block_levels[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        printf("  History:    ");
        for (size_t i = 0; i < history_count; ++i) {
            int level = (history[i] + 100) / 10;
            if (level < 0)
                level = 0;
            if (level > 9)
                level = 9;
            if (wifi_unicode())
                fputs(block_levels[level > 7 ? 7 : level], stdout);
            else
                putchar(ascii_levels[level]);
        }
        putchar('\n');
    }
}
#endif

static cli_cmd_result cmd_wifi_signal(char **tokens, size_t count)
{
    int live = 0;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--live") == 0)
            live = 1;
        else
            return CLI_CMD_INVALID;
    }

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)live;
    puts("\nwifi signal: live signal diagnostics are unavailable in this offline build.");
    puts("Use `wifi status` for build and adapter metadata.");
    return CLI_CMD_CONTINUE;
#else
    if (!live) {
        nero_wifi_platform_status status;
        char error[256] = {0};
        if (nero_wifi_platform_get_status(&status, error, sizeof(error)) != 0) {
            printf("\nwifi signal unavailable: %s\n",
                   error[0] != '\0' ? error : "unknown error");
            return CLI_CMD_CONTINUE;
        }
        print_signal_snapshot(&status, NULL, 0);
        puts("\n  Use `wifi signal --live` for a refreshing panel (Ctrl+C stops).");
        puts("  Observation is read-only; no metrics are fabricated when the backend omits them.");
        return CLI_CMD_CONTINUE;
    }

    int history[NERO_WIFI_SIGNAL_HISTORY];
    size_t history_count = 0;
    for (;;) {
        nero_wifi_platform_status status;
        char error[256] = {0};
        if (nero_wifi_platform_get_status(&status, error, sizeof(error)) != 0) {
            printf("\nwifi signal unavailable: %s\n",
                   error[0] != '\0' ? error : "unknown error");
            break;
        }
        if (terminal_ansi_enabled())
            clear_screen();
        print_signal_snapshot(&status, history, history_count);
        if (status.has_rssi)
            signal_history_add(history, &history_count, status.rssi);
        puts("\n  Ctrl+C stops. Noise/SNR/bitrate are shown only when the backend exposes them.");
        fflush(stdout);
        if (cli_termination_signal != 0)
            break;
        sleep_milliseconds(1000);
    }
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;
    return CLI_CMD_CONTINUE;
#endif
}

static const nero_table_col interface_cols[] = {
    {"NAME", 6, 14, 1, 0, 0, 12, NERO_TABLE_TEXT},
    {"HARDWARE ADDRESS", 17, 18, 0, 0, 4, 0, NERO_TABLE_TEXT},
    {"POWER", 5, 7, 0, 0, 3, 0, NERO_TABLE_TEXT},
    {"SERVICE", 6, 8, 0, 0, 2, 0, NERO_TABLE_TEXT},
    {"CHANNEL", 7, 7, 0, 1, 1, 0, NERO_TABLE_TEXT},
    {"MODE", 4, 4, 1, 0, 0, 0, NERO_TABLE_TEXT},
};

static void interface_row_build(int index, void *ctx, nero_table_cell *out)
{
    const nero_wifi_interface_info *info = &((const nero_wifi_interface_info *)ctx)[index];
    char channel[16];
    char mode[16];
    if (info->has_channel)
        snprintf(channel, sizeof(channel), "%d", info->channel);
    else
        snprintf(channel, sizeof(channel), "-");
    snprintf(mode, sizeof(mode), "%d", info->mode);

    snprintf(out[0].text, sizeof(out[0].text), "%s", info->name);
    snprintf(out[1].text, sizeof(out[1].text), "%s",
             info->hardware_address[0] != '\0' ? info->hardware_address : "unknown");
    snprintf(out[2].text, sizeof(out[2].text), "%s", info->powered ? "on" : "off");
    out[2].color = info->powered ? WIFI_GREEN : WIFI_DIM;
    snprintf(out[3].text, sizeof(out[3].text), "%s",
             info->service_active ? "active" : "inactive");
    out[3].color = info->service_active ? WIFI_GREEN : WIFI_DIM;
    snprintf(out[4].text, sizeof(out[4].text), "%s", channel);
    snprintf(out[5].text, sizeof(out[5].text), "%s", mode);
    out[5].color = WIFI_DIM;
}

static cli_cmd_result cmd_wifi_interfaces(void)
{
    nero_wifi_interface_info interfaces[16];
    int count = 0;
    char error[256] = {0};
    if (nero_wifi_platform_list_interfaces(interfaces, 16, &count, error, sizeof(error)) != 0) {
        printf("\nWi-Fi interfaces unavailable: %s\n", error[0] != '\0' ? error : "unknown error");
        return CLI_CMD_CONTINUE;
    }
    printf("\n%sWI-FI INTERFACES%s\n", wc(WIFI_HEAD), wc_off());
    if (count == 0) {
        printf("  %s(no Wi-Fi interface detected)%s\n", wc(WIFI_DIM), wc_off());
        return CLI_CMD_CONTINUE;
    }
    nero_table_opts opts;
    wifi_table_opts(&opts);
    nero_table_draw(stdout, &opts, interface_cols,
                    (int)(sizeof(interface_cols) / sizeof(interface_cols[0])), count,
                    interface_row_build, interfaces);
    return CLI_CMD_CONTINUE;
}

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
static int ap_sort_compare(const void *left, const void *right)
{
    const nero_wifi_ap *a = left;
    const nero_wifi_ap *b = right;
    int by_ssid = strcmp(a->ssid, b->ssid);
    if (by_ssid != 0)
        return by_ssid;
    return memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
}
#endif

static cli_cmd_result cmd_wifi_scan(char **tokens, size_t count)
{
    const char *interface = NULL;
    nero_wifi_band band = NERO_WIFI_BAND_UNKNOWN;
    int channel = -1;
    int live = 0;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--live") == 0) {
            live = 1;
        } else if (strcmp(tokens[i], "--band") == 0 && i + 1 < count) {
            if (parse_band(tokens[++i], &band) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 1, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (tokens[i][0] == '-') {
            return CLI_CMD_INVALID;
        } else if (interface == NULL) {
            interface = tokens[i];
        } else {
            return CLI_CMD_INVALID;
        }
    }

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
    (void)band;
    (void)channel;
    (void)live;
    puts("\nwifi scan: unsupported in this offline build; no active scan was performed.");
    return CLI_CMD_CONTINUE;
#else
    if (wifi_session != NULL && nero_wifi_session_running(wifi_session)) {
        puts("\nWARNING: an active scan may interrupt the passive capture; stop it first for");
        puts("stable monitor results.");
    }
    nero_wifi_ap *aps = calloc(256, sizeof(*aps));
    if (aps == NULL) {
        fputs("\nwifi scan: out of memory\n", stderr);
        return CLI_CMD_FAILURE;
    }
    for (;;) {
        int count_aps = 0;
        nero_wifi_platform_capabilities capabilities;
        char error[256] = {0};
        if (nero_wifi_platform_scan(interface, aps, 256, &count_aps, &capabilities, error,
                                    sizeof(error)) != 0) {
            printf("\nwifi scan failed: %s\n", error[0] != '\0' ? error : "unknown error");
            if (!live)
                break;
            puts("Active scanning is unavailable; no fabricated results are shown.");
            break;
        }
        count_aps = filter_aps_by_radio(aps, count_aps, band, channel);
        qsort(aps, (size_t)count_aps, sizeof(aps[0]), ap_sort_compare);

        if (live && terminal_ansi_enabled())
            clear_screen();
        char now_text[32];
        nero_wifi_format_time(nero_wifi_now_us(), now_text, sizeof(now_text));
        if (live) {
            printf("\nACTIVE WIRELESS SURVEY ][ %s ][ %s ][ rescan 5s ][ Ctrl+C stops ]\n",
                   interface != NULL ? interface : "default interface", now_text);
        } else {
            puts("\nACTIVE WIRELESS SURVEY (CoreWLAN; no network was joined)");
        }
        if (band != NERO_WIFI_BAND_UNKNOWN || channel >= 0) {
            printf("  Filter:");
            if (band != NERO_WIFI_BAND_UNKNOWN)
                printf(" band %s", nero_wifi_band_name(band));
            if (channel >= 0)
                printf(" channel %d", channel);
            putchar('\n');
        }
        if (capabilities.identifiers_redacted) {
            puts("  NOTE: macOS redacted one or more SSID/BSSID values. Grant Location Services");
            puts("        to the terminal application to reveal identifiers. Hidden values are");
            puts("        never invented.");
        }
        print_ap_table(aps, count_aps);
        printf("\n  %d access point(s) reported by the active scan.\n", count_aps);
        if (count_aps == 0 && (band != NERO_WIFI_BAND_UNKNOWN || channel >= 0)) {
            puts("  No access point matched the filter. The adapter or regulatory domain may");
            puts("  not support the requested band/channel, or no network was audible.");
        }
        if (!live) {
            puts("  Active survey results are separate from passive session data; use `wifi analyze`");
            puts("  or `wifi monitor` to populate data shown by `wifi aps` and `wifi export`.");
            break;
        }
        puts("  Active scans transmit normal probe requests. No network is joined.");
        fflush(stdout);
        if (cli_termination_signal != 0)
            break;
        sleep_milliseconds(5000);
        if (cli_termination_signal != 0)
            break;
    }
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;
    free(aps);
    return CLI_CMD_CONTINUE;
#endif
}

static cli_cmd_result cmd_wifi_aps(char **tokens, size_t count)
{
    int channel = -1;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else {
            return CLI_CMD_INVALID;
        }
    }
    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;
    nero_wifi_ap aps[NERO_WIFI_MAX_APS];
    int aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, channel);
    print_ap_table(aps, aps_count);
    if (channel >= 0)
        printf("\n  Channel filter: %d\n", channel);
    print_channel_summary(aps, aps_count);
    return CLI_CMD_CONTINUE;
}

static cli_cmd_result cmd_wifi_clients(char **tokens, size_t count)
{
    int channel = -1;
    int anonymize = 0;
    int live = 0;
    uint8_t bssid_filter[NERO_WIFI_MAC_LEN];
    int has_bssid_filter = 0;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--bssid") == 0 && i + 1 < count) {
            if (nero_wifi_parse_mac(tokens[++i], bssid_filter) != 0)
                return CLI_CMD_INVALID;
            has_bssid_filter = 1;
        } else if (strcmp(tokens[i], "--anonymize") == 0) {
            anonymize = 1;
        } else if (strcmp(tokens[i], "--live") == 0) {
            live = 1;
        } else {
            return CLI_CMD_INVALID;
        }
    }
    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;

    if (live) {
        nero_wifi_stats stats;
        nero_wifi_session_snapshot_stats(session, &stats);
        if (!nero_wifi_session_running(session) && stats.packets == 0) {
            puts("\nwifi clients --live: no passive data is available.");
            puts("Start `wifi monitor start` or import a capture with `wifi analyze <PCAP_FILE>`.");
            return CLI_CMD_CONTINUE;
        }
    }

    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (clients == NULL)
        return CLI_CMD_FAILURE;
    char bssid_text[32] = {0};
    if (has_bssid_filter)
        nero_wifi_format_mac(bssid_filter, bssid_text, sizeof(bssid_text));

    for (;;) {
        int client_count =
            nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, channel);
        int kept = 0;
        for (int i = 0; i < client_count; ++i) {
            if (has_bssid_filter &&
                (!clients[i].has_bssid ||
                 memcmp(clients[i].bssid, bssid_filter, NERO_WIFI_MAC_LEN) != 0)) {
                continue;
            }
            if (kept != i)
                clients[kept] = clients[i];
            ++kept;
        }
        client_count = kept;

        if (live && terminal_ansi_enabled())
            clear_screen();
        print_client_table(clients, client_count, anonymize);
        printf("\n  %d client device(s) observed. Observation is not proof of a live connection.\n",
               client_count);
        if (channel >= 0)
            printf("  Channel filter: %d\n", channel);
        if (has_bssid_filter)
            printf("  BSSID filter:   %s\n", bssid_text);
        if (live)
            puts("  Refreshing every 1s; Ctrl+C stops.");
        fflush(stdout);
        if (!live)
            break;
        if (cli_termination_signal != 0)
            break;
        sleep_milliseconds(1000);
        if (cli_termination_signal != 0)
            break;
    }
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;
    free(clients);
    return CLI_CMD_CONTINUE;
}

static void rssi_sparkline(const int *history, uint8_t count, uint8_t next, char *out,
                           size_t out_len)
{
    static const char levels[] = " .:-=+*#%@";
    size_t written = 0;
    if (out_len == 0)
        return;
    for (uint8_t i = 0; i < count && written + 1 < out_len; ++i) {
        uint8_t index =
            (uint8_t)((next + NERO_WIFI_RSSI_HISTORY - count + i) % NERO_WIFI_RSSI_HISTORY);
        int value = history[index];
        int level = (value + 100) / 10;
        if (level < 0)
            level = 0;
        if (level > 9)
            level = 9;
        out[written++] = levels[level];
    }
    out[written] = '\0';
}

static cli_cmd_result cmd_wifi_inspect(char **tokens, size_t count)
{
    if (count != 2)
        return CLI_CMD_INVALID;
    uint8_t mac[NERO_WIFI_MAC_LEN];
    if (nero_wifi_parse_mac(tokens[1], mac) != 0) {
        fputs("\nInvalid MAC address. Use aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff or 12 hex digits.\n",
              stderr);
        return CLI_CMD_INVALID;
    }
    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;

    nero_wifi_client client;
    if (nero_wifi_session_find_client(session, mac, &client) == 0) {
        char mac_text[32];
        char bssid_text[32];
        char first[32], last[32], age[24], spark[32];
        char channel_text[32];
        char rssi_text[32];
        nero_wifi_format_mac(client.mac, mac_text, sizeof(mac_text));
        display_client_bssid(&client, bssid_text, sizeof(bssid_text));
        nero_wifi_format_time(client.first_seen_us, first, sizeof(first));
        nero_wifi_format_time(client.last_seen_us, last, sizeof(last));
        format_age(&client, nero_wifi_now_us(), age, sizeof(age));
        if (client.has_rssi)
            snprintf(rssi_text, sizeof(rssi_text), "%d dBm", client.rssi);
        else
            snprintf(rssi_text, sizeof(rssi_text), "unavailable");
        if (client.has_channel) {
            snprintf(channel_text, sizeof(channel_text), "%d (%s)", client.channel,
                     nero_wifi_band_name(client.band));
        } else {
            snprintf(channel_text, sizeof(channel_text), "unknown");
        }
        rssi_sparkline(client.rssi_history, client.rssi_history_count, client.rssi_history_next,
                       spark, sizeof(spark));
        printf("\nCLIENT DEVICE\n"
               "  MAC:            %s\n"
               "  Randomized:     %s\n"
               "  BSSID:          %s\n"
               "  SSID:           %s\n"
               "  Confidence:     %s\n"
               "  State:          %s\n"
               "  RSSI:           %s\n"
               "  RSSI history:   %s\n"
               "  Channel:        %s\n"
               "  First seen:     %s\n"
               "  Last seen:      %s (idle %s)\n"
               "  Frames:         %u (mgmt %u, data %u, duplicates %u)\n"
               "  AP changes:     %u\n",
               mac_text, client.randomized ? "yes (locally administered)" : "no", bssid_text,
               client.ssid[0] != '\0' ? client.ssid : "<hidden/unknown>",
               nero_wifi_confidence_name(client.confidence), nero_wifi_state_name(client.state),
               rssi_text, spark, channel_text, first, last, age, client.frames,
               client.management_frames, client.data_frames, client.duplicates, client.ap_changes);
        puts("\n  Evidence note: ASSOC-REQ means an association request was observed. It does not");
        puts("  prove the AP accepted or currently maintains the association.");
        return CLI_CMD_CONTINUE;
    }

    nero_wifi_ap ap;
    if (nero_wifi_session_find_ap(session, mac, &ap) == 0) {
        char ssid[64], bssid[32];
        char first[32], last[32];
        char channel_text[32];
        char frequency_text[32];
        char rssi_text[32];
        display_ssid(&ap, ssid, sizeof(ssid));
        display_bssid(&ap, bssid, sizeof(bssid));
        nero_wifi_format_time(ap.first_seen_us, first, sizeof(first));
        nero_wifi_format_time(ap.last_seen_us, last, sizeof(last));
        if (ap.has_channel) {
            snprintf(channel_text, sizeof(channel_text), "%d (%s)", ap.channel,
                     nero_wifi_band_name(ap.band));
        } else {
            snprintf(channel_text, sizeof(channel_text), "unknown");
        }
        if (ap.has_frequency)
            snprintf(frequency_text, sizeof(frequency_text), "%d MHz", ap.frequency_mhz);
        else
            snprintf(frequency_text, sizeof(frequency_text), "unknown");
        if (ap.has_rssi)
            snprintf(rssi_text, sizeof(rssi_text), "%d dBm", ap.rssi);
        else
            snprintf(rssi_text, sizeof(rssi_text), "unavailable");
        printf("\nACCESS POINT\n"
               "  SSID:           %s\n"
               "  BSSID:          %s\n"
               "  Channel:        %s\n"
               "  Band:           %s\n"
               "  Frequency:      %s\n"
               "  RSSI:           %s\n"
               "  Security:       %s\n"
               "  First seen:     %s\n"
               "  Last seen:      %s\n"
               "  Beacons:        %u\n"
               "  Probe resp:     %u\n"
               "  Client count:   %d (observed %d)\n",
               ssid, bssid, channel_text, nero_wifi_band_name(ap.band), frequency_text, rssi_text,
               nero_wifi_security_name(ap.security), first, last, ap.beacon_frames,
               ap.probe_response_frames, ap.client_count, ap.observed_client_count);

        nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
        if (clients != NULL && ap.has_bssid) {
            int client_count =
                nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, -1);
            puts("\n  OBSERVED CLIENTS AT THIS BSSID");
            print_client_rows(clients, client_count, 0, ap.bssid);
        }
        free(clients);
        return CLI_CMD_CONTINUE;
    }

    printf("\nNo observed client or access point matches %s in the current data set.\n", tokens[1]);
    puts("The address may be absent, randomized, or on another channel not captured.");
    return CLI_CMD_CONTINUE;
}

static cli_cmd_result cmd_wifi_analyze(char **tokens, size_t count)
{
    if (count != 2)
        return CLI_CMD_INVALID;
    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;
    char error[256] = {0};
    if (nero_wifi_session_analyze(session, tokens[1], error, sizeof(error)) != 0) {
        printf("\nwifi analyze failed: %s\n", error[0] != '\0' ? error : "unknown error");
        puts("The existing session data set was not changed.");
        return CLI_CMD_CONTINUE;
    }
    nero_wifi_stats stats;
    nero_wifi_session_snapshot_stats(session, &stats);
    printf("\nOFFLINE CAPTURE ANALYSIS\n  File: %s\n", tokens[1]);
    print_stats_summary(&stats);

    nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (aps == NULL || clients == NULL) {
        free(aps);
        free(clients);
        return CLI_CMD_FAILURE;
    }
    int aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, -1);
    int client_count = nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, -1);
    print_ap_table(aps, aps_count);
    print_client_table(clients, client_count, 0);
    print_channel_summary(aps, aps_count);
    if (stats.frames > 0 && aps_count == 0) {
        puts("\n  NOTE: frames were parsed but no beacon/probe-response AP record was found.");
        puts("  The capture may contain only client traffic or a non-infrastructure channel.");
    }
    if (stats.malformed > 0) {
        printf("\n  WARNING: %llu malformed frame(s) were skipped safely.\n",
               (unsigned long long)stats.malformed);
    }
    free(aps);
    free(clients);
    return CLI_CMD_CONTINUE;
}

static uint64_t anonymization_salt(void)
{
    static uint64_t salt;
    if (salt == 0) {
        salt = (uint64_t)nero_wifi_now_us() ^ 0x9e3779b97f4a7c15ULL;
#if defined(_WIN32)
        salt ^= (uint64_t)GetCurrentProcessId() << 32;
#else
        salt ^= (uint64_t)getpid() << 32;
#endif
    }
    return salt;
}

static void anonymize_mac_address(const uint8_t input[NERO_WIFI_MAC_LEN],
                                  uint8_t output[NERO_WIFI_MAC_LEN])
{
    uint64_t hash = 1469598103934665603ULL ^ anonymization_salt();
    for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i) {
        hash ^= input[i];
        hash *= 1099511628211ULL;
    }
    for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i)
        output[i] = (uint8_t)(hash >> (i * 8));
    output[0] = (uint8_t)((output[0] & 0xfeu) | 0x02u);
}

static void csv_field(FILE *file, const char *text)
{
    fputc('"', file);
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == '"')
            fputc('"', file);
        fputc(*p, file);
    }
    fputc('"', file);
}

static cli_cmd_result cmd_wifi_export(char **tokens, size_t count)
{
    int channel = -1;
    int anonymize = 0;
    const char *path = NULL;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--anonymize") == 0) {
            anonymize = 1;
        } else if (tokens[i][0] == '-') {
            return CLI_CMD_INVALID;
        } else if (path == NULL) {
            path = tokens[i];
        } else {
            return CLI_CMD_INVALID;
        }
    }
    if (path == NULL)
        path = "nero-wifi-export.csv";

    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;
    nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (aps == NULL || clients == NULL) {
        free(aps);
        free(clients);
        return CLI_CMD_FAILURE;
    }
    int aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, channel);
    int client_count =
        nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, channel);
    /* The export holds MAC addresses, so create it with owner-only permissions
     * (0600) rather than fopen's umask-dependent 0666 on POSIX. */
#if defined(_WIN32)
    FILE *file = fopen(path, "w");
#else
    int export_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *file = export_fd >= 0 ? fdopen(export_fd, "w") : NULL;
    if (file == NULL && export_fd >= 0)
        close(export_fd);
#endif
    if (file == NULL) {
        printf("\nwifi export failed: %s: %s\n", path, strerror(errno));
        free(aps);
        free(clients);
        return CLI_CMD_CONTINUE;
    }
    fputs("type,mac,bssid,ssid,channel,band,rssi,security,confidence,state,first_seen,last_seen,"
          "frames,management,data,duplicates,randomized\n",
          file);
    for (int i = 0; i < aps_count; ++i) {
        uint8_t bssid[NERO_WIFI_MAC_LEN];
        char bssid_text[32] = "";
        if (aps[i].has_bssid) {
            memcpy(bssid, aps[i].bssid, NERO_WIFI_MAC_LEN);
            if (anonymize)
                anonymize_mac_address(bssid, bssid);
            nero_wifi_format_mac(bssid, bssid_text, sizeof(bssid_text));
        }
        char first[32], last[32];
        nero_wifi_format_time(aps[i].first_seen_us, first, sizeof(first));
        nero_wifi_format_time(aps[i].last_seen_us, last, sizeof(last));
        fprintf(file, "ap,,%s,", bssid_text);
        csv_field(file, aps[i].ssid);
        fprintf(file, ",%d,%s,%d,%s,,,%s,%s,,,,,\n", aps[i].channel,
                nero_wifi_band_name(aps[i].band), aps[i].rssi,
                nero_wifi_security_name(aps[i].security), first, last);
    }
    for (int i = 0; i < client_count; ++i) {
        uint8_t mac[NERO_WIFI_MAC_LEN];
        uint8_t bssid[NERO_WIFI_MAC_LEN];
        char mac_text[32], bssid_text[32] = "";
        memcpy(mac, clients[i].mac, NERO_WIFI_MAC_LEN);
        if (anonymize)
            anonymize_mac_address(mac, mac);
        nero_wifi_format_mac(mac, mac_text, sizeof(mac_text));
        if (clients[i].has_bssid) {
            memcpy(bssid, clients[i].bssid, NERO_WIFI_MAC_LEN);
            if (anonymize)
                anonymize_mac_address(bssid, bssid);
            nero_wifi_format_mac(bssid, bssid_text, sizeof(bssid_text));
        }
        char first[32], last[32];
        nero_wifi_format_time(clients[i].first_seen_us, first, sizeof(first));
        nero_wifi_format_time(clients[i].last_seen_us, last, sizeof(last));
        fprintf(file, "client,%s,%s,", mac_text, bssid_text);
        csv_field(file, clients[i].ssid);
        fprintf(file, ",%d,%s,%d,,%s,%s,%s,%s,%u,%u,%u,%u,%s\n",
                clients[i].has_channel ? clients[i].channel : 0,
                nero_wifi_band_name(clients[i].band), clients[i].has_rssi ? clients[i].rssi : 0,
                nero_wifi_confidence_name(clients[i].confidence),
                nero_wifi_state_name(clients[i].state), first, last, clients[i].frames,
                clients[i].management_frames, clients[i].data_frames, clients[i].duplicates,
                clients[i].randomized ? "yes" : "no");
    }
    if (fclose(file) != 0) {
        printf("\nwifi export failed while writing %s: %s\n", path, strerror(errno));
        free(aps);
        free(clients);
        return CLI_CMD_CONTINUE;
    }
    printf("\nExported %d access point(s) and %d client(s) to %s%s.\n", aps_count, client_count,
           path, anonymize ? " (addresses pseudonymized)" : "");
    puts("Observed MAC addresses are stored only because you explicitly exported them.");
    free(aps);
    free(clients);
    return CLI_CMD_CONTINUE;
}

static void sleep_milliseconds(int milliseconds)
{
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&delay, NULL);
#endif
}

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
/* The live monitor survey renders through the shared table engine, so the live
 * dashboard and the inline (piped/CI) fallback share one responsive grid. These
 * column specs add the survey-specific beacon/probe/frame counters on top of
 * the common signal/band/security vocabulary. */
static const nero_table_col monitor_ap_cols[] = {
    {"SIGNAL", 9, 9, 0, 0, 0, 0, NERO_TABLE_SIGNAL},
    {"BSSID", 17, 17, 0, 0, 6, 0, NERO_TABLE_TEXT},
    {"BEACONS", 7, 7, 0, 1, 2, 0, NERO_TABLE_TEXT},
    {"PROBES", 6, 6, 0, 1, 1, 0, NERO_TABLE_TEXT},
    {"CH", 3, 3, 0, 1, 5, 0, NERO_TABLE_TEXT},
    {"BAND", 5, 7, 0, 0, 3, 0, NERO_TABLE_TEXT},
    {"SECURITY", 6, 10, 0, 0, 4, 0, NERO_TABLE_TEXT},
    {"CLIENTS", 7, 7, 0, 1, 2, 0, NERO_TABLE_TEXT},
    {"ESSID", 10, 24, 1, 0, 0, 32, NERO_TABLE_TEXT},
};

static void monitor_ap_row_build(int index, void *ctx, nero_table_cell *out)
{
    const nero_wifi_ap *ap = &((const nero_wifi_ap *)ctx)[index];
    char bssid[32];
    char ssid[64];
    int hidden = ap->hidden || ap->redacted || ap->ssid_len == 0;

    out[0].has_rssi = ap->has_rssi;
    out[0].rssi = ap->rssi;
    snprintf(out[1].text, sizeof(out[1].text), "%s", display_bssid(ap, bssid, sizeof(bssid)));
    out[1].color = ap->has_bssid ? NULL : WIFI_DIM;
    snprintf(out[2].text, sizeof(out[2].text), "%u", ap->beacon_frames);
    out[2].color = WIFI_DIM;
    snprintf(out[3].text, sizeof(out[3].text), "%u", ap->probe_response_frames);
    out[3].color = WIFI_DIM;
    if (ap->has_channel)
        snprintf(out[4].text, sizeof(out[4].text), "%d", ap->channel);
    else
        snprintf(out[4].text, sizeof(out[4].text), "-");
    snprintf(out[5].text, sizeof(out[5].text), "%s",
             ap->band == NERO_WIFI_BAND_UNKNOWN ? "-" : nero_wifi_band_name(ap->band));
    out[5].color = band_color(ap->band);
    snprintf(out[6].text, sizeof(out[6].text), "%s", nero_wifi_security_name(ap->security));
    out[6].color = security_color(ap->security);
    snprintf(out[7].text, sizeof(out[7].text), "%d", ap->client_count);
    snprintf(out[8].text, sizeof(out[8].text), "%s", display_ssid(ap, ssid, sizeof(ssid)));
    out[8].color = hidden ? WIFI_DIM : NULL;
}

static const nero_table_col monitor_client_cols[] = {
    {"BSSID", 17, 17, 0, 0, 5, 0, NERO_TABLE_TEXT},
    {"STATION", 17, 17, 0, 0, 0, 0, NERO_TABLE_TEXT},
    {"SIGNAL", 9, 9, 0, 0, 0, 0, NERO_TABLE_SIGNAL},
    {"CH", 3, 3, 0, 1, 4, 0, NERO_TABLE_TEXT},
    {"FRAMES", 6, 6, 0, 1, 2, 0, NERO_TABLE_TEXT},
    {"NOTES", 6, 16, 1, 0, 1, 10, NERO_TABLE_TEXT},
    {"ESSID", 8, 24, 1, 0, 3, 20, NERO_TABLE_TEXT},
};

static void airodump_client_notes(const nero_wifi_client *client, char *out, size_t out_len);

static void monitor_client_row_build(int index, void *ctx, nero_table_cell *out)
{
    const nero_wifi_client *client = &((const nero_wifi_client *)ctx)[index];
    char bssid[32];
    char mac[32];
    char notes[32];

    snprintf(out[0].text, sizeof(out[0].text), "%s",
             display_client_bssid(client, bssid, sizeof(bssid)));
    nero_wifi_format_mac(client->mac, mac, sizeof(mac));
    snprintf(out[1].text, sizeof(out[1].text), "%s", mac);
    out[1].color = client->randomized ? WIFI_DIM : NULL;
    out[2].has_rssi = client->has_rssi;
    out[2].rssi = client->rssi;
    if (client->has_channel)
        snprintf(out[3].text, sizeof(out[3].text), "%d", client->channel);
    else
        snprintf(out[3].text, sizeof(out[3].text), "-");
    snprintf(out[4].text, sizeof(out[4].text), "%u", client->frames);
    out[4].color = WIFI_DIM;
    airodump_client_notes(client, notes, sizeof(notes));
    snprintf(out[5].text, sizeof(out[5].text), "%s", notes);
    out[5].color = notes[0] != '\0' ? WIFI_YEL : NULL;
    snprintf(out[6].text, sizeof(out[6].text), "%s",
             client->ssid[0] != '\0' ? client->ssid : "-");
    out[6].color = client->ssid[0] != '\0' ? NULL : WIFI_DIM;
}

static int ap_power_compare(const void *left, const void *right)
{
    const nero_wifi_ap *a = left;
    const nero_wifi_ap *b = right;
    int ap = a->has_rssi ? a->rssi : -1000;
    int bp = b->has_rssi ? b->rssi : -1000;
    if (ap != bp)
        return bp - ap;
    return memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
}

static int client_bssid_compare(const void *left, const void *right)
{
    const nero_wifi_client *a = left;
    const nero_wifi_client *b = right;
    int by_bssid = memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
    if (by_bssid != 0)
        return by_bssid;
    return memcmp(a->mac, b->mac, NERO_WIFI_MAC_LEN);
}

static void airodump_client_notes(const nero_wifi_client *client, char *out, size_t out_len)
{
    out[0] = '\0';
    if (client->state == NERO_WIFI_STATE_DISASSOCIATED)
        strncat(out, "LEFT ", out_len - strlen(out) - 1);
    if (client->randomized)
        strncat(out, "RANDOM ", out_len - strlen(out) - 1);
    if (client->confidence == NERO_WIFI_CONFIDENCE_ASSOCIATED)
        strncat(out, "ASSOC ", out_len - strlen(out) - 1);
    else if (client->confidence == NERO_WIFI_CONFIDENCE_LIKELY)
        strncat(out, "LIKELY ", out_len - strlen(out) - 1);
    size_t used = strlen(out);
    if (used > 0 && out[used - 1] == ' ')
        out[used - 1] = '\0';
}

static int ap_essid_compare(const void *left, const void *right)
{
    const nero_wifi_ap *a = left;
    const nero_wifi_ap *b = right;
    int ah = a->hidden || a->ssid_len == 0;
    int bh = b->hidden || b->ssid_len == 0;
    if (ah != bh)
        return ah - bh; /* named APs first, hidden last */
    int by_ssid = strncmp(a->ssid, b->ssid, sizeof(a->ssid));
    if (by_ssid != 0)
        return by_ssid;
    return memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
}

static int ap_bssid_compare(const void *left, const void *right)
{
    const nero_wifi_ap *a = left;
    const nero_wifi_ap *b = right;
    return memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
}

/* Fills render options for the live survey: a full-width grid (no 120-cell cap)
 * so the ESSID column can use every column the terminal offers. */
static void wifi_monitor_opts(nero_table_opts *opts, int width)
{
    nero_table_opts_default(opts);
    if (width < 24)
        width = 24;
    opts->width = width;
    opts->max_width = width;
}

/* Writes one complete survey frame (status bar + AP grid + station grid +
 * honest capture warnings) into `out`. Used for both the live alternate-screen
 * TUI (out = an in-memory stream that is then diffed) and the inline/piped
 * fallback (out = stdout), so a single engine renders every surface. */
static void survey_build_frame(FILE *out, const nero_table_opts *opts, nero_wifi_session *session,
                               const nero_wifi_stats *stats, const nero_wifi_ap *aps, int aps_count,
                               const nero_wifi_client *clients, int client_count,
                               int elapsed_seconds, double pps, int channel_filter, int paused,
                               const char *sort_label, int clients_only)
{
    char interface[NERO_WIFI_IFNAME_MAX] = {0};
    char now_text[32];
    char channel[16];
    nero_wifi_session_interface(session, interface, sizeof(interface));
    nero_wifi_format_time(nero_wifi_now_us(), now_text, sizeof(now_text));
    if (channel_filter >= 0)
        snprintf(channel, sizeof(channel), "%d", channel_filter);
    else
        snprintf(channel, sizeof(channel), "mon");

    fprintf(out,
            " %sNERO WIFI SURVEY%s  CH %s | %ds | %s | IFACE %s | %s | %llu pkt | %.1f/s | drop %llu%s\n",
            wc(WIFI_HEAD), wc_off(), channel, elapsed_seconds, now_text,
            interface[0] != '\0' ? interface : "(unknown)",
            stats->monitor_requested ? "MONITOR" : "PASSIVE", (unsigned long long)stats->packets,
            pps, (unsigned long long)stats->dropped, paused ? "  [PAUSED]" : "");
    if (stats->malformed > 0 || stats->non_wifi > 0)
        fprintf(out, " %smalformed %llu, non-802.11 %llu%s\n", wc(WIFI_DIM),
                (unsigned long long)stats->malformed, (unsigned long long)stats->non_wifi, wc_off());

    fprintf(out, "\n %sACCESS POINTS%s %s(%d, sort: %s)%s\n", wc(WIFI_HEAD), wc_off(), wc(WIFI_DIM),
            aps_count, sort_label != NULL ? sort_label : "power", wc_off());
    if (aps_count == 0)
        fprintf(out, " %s(no access points observed yet: waiting for beacon/probe frames)%s\n",
                wc(WIFI_DIM), wc_off());
    else
        nero_table_draw(out, opts, monitor_ap_cols,
                        (int)(sizeof(monitor_ap_cols) / sizeof(monitor_ap_cols[0])), aps_count,
                        monitor_ap_row_build, (void *)aps);

    fprintf(out, "\n %sSTATIONS%s %s(%d%s)%s\n", wc(WIFI_HEAD), wc_off(), wc(WIFI_DIM), client_count,
            clients_only ? ", filtered" : "", wc_off());
    if (client_count == 0)
        fprintf(out, " %s(no client stations observed yet)%s\n", wc(WIFI_DIM), wc_off());
    else
        nero_table_draw(out, opts, monitor_client_cols,
                        (int)(sizeof(monitor_client_cols) / sizeof(monitor_client_cols[0])),
                        client_count, monitor_client_row_build, (void *)clients);

    if (stats->packets == 0 && elapsed_seconds >= 3) {
        fprintf(out, "\n %sWARNING: monitor mode was accepted but zero 802.11 frames arrived.%s\n",
                wc(WIFI_YEL), wc_off());
        fprintf(out, " %sOn Apple Silicon the built-in adapter may not expose monitor capture;%s\n",
                wc(WIFI_DIM), wc_off());
        fprintf(out, " %suse a genuine monitor-mode pcap with `wifi analyze` in that case.%s\n",
                wc(WIFI_DIM), wc_off());
    }
    if (stats->dropped > 0)
        fprintf(out, "\n %sCapture loss: %llu packet(s) dropped by the kernel/backend.%s\n",
                wc(WIFI_DIM), (unsigned long long)stats->dropped, wc_off());
}

/* Fetch + sort + optional filter of the current survey snapshot. Returns the
 * displayed AP/client counts through the out params. */
static void survey_collect(nero_wifi_session *session, int channel_filter, int sort_mode,
                           int clients_only, nero_wifi_ap *aps, int *aps_count,
                           nero_wifi_client *clients, int *client_count)
{
    *aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, channel_filter);
    *client_count =
        nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, channel_filter);
    if (sort_mode == 1)
        qsort(aps, (size_t)*aps_count, sizeof(aps[0]), ap_essid_compare);
    else if (sort_mode == 2)
        qsort(aps, (size_t)*aps_count, sizeof(aps[0]), ap_bssid_compare);
    else
        qsort(aps, (size_t)*aps_count, sizeof(aps[0]), ap_power_compare);
    qsort(clients, (size_t)*client_count, sizeof(clients[0]), client_bssid_compare);
    if (clients_only) {
        int w = 0;
        for (int i = 0; i < *client_count; ++i)
            if (clients[i].confidence != NERO_WIFI_CONFIDENCE_OBSERVED)
                clients[w++] = clients[i];
        *client_count = w;
    }
}

static const char *survey_sort_label(int sort_mode)
{
    return sort_mode == 1 ? "essid" : (sort_mode == 2 ? "bssid" : "power");
}

/* ---- opt-in channel hopping -------------------------------------------- *
 * Default `wifi monitor` is strictly read-only and never changes the radio.
 * `wifi monitor --hop` is a separate, explicitly consented mode that tunes the
 * monitor interface across a channel plan so a survey can see every channel,
 * not just the one the adapter currently sits on. Changing the channel needs a
 * monitor-mode interface and CAP_NET_ADMIN/root; it is only implemented on
 * Linux (nl80211) and reported honestly as unavailable elsewhere. */
typedef struct {
    int enabled;
    int freqs[96];
    int count;
    int idx;
    int dwell_ms;
    int64_t last_hop_us;
    char iface[NERO_WIFI_IFNAME_MAX];
} wifi_hop_ctx;

/* The active hop context, consulted by the live dashboard loops. NULL means the
 * default read-only single-channel survey. */
static wifi_hop_ctx *active_hop;

#if defined(__linux__)
/* Builds a conservative channel plan for the requested bands. 2.4 GHz uses the
 * universally present channels 1-13; 5/6 GHz use the common non-DFS-leaning
 * channels. The kernel rejects channels the regulatory domain forbids, which
 * the hop loop tolerates. Only the Linux hop path consumes this. */
static int wifi_build_freq_plan(unsigned bands, int *freqs, int cap)
{
    int n = 0;
    if (bands & WIFI_HOP_2GHZ) {
        for (int ch = 1; ch <= 13 && n < cap; ++ch)
            freqs[n++] = 2412 + (ch - 1) * 5;
    }
    if (bands & WIFI_HOP_5GHZ) {
        static const int ch5[] = {36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108, 112,
                                  116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165};
        for (size_t i = 0; i < sizeof(ch5) / sizeof(ch5[0]) && n < cap; ++i)
            freqs[n++] = 5000 + ch5[i] * 5;
    }
    if (bands & WIFI_HOP_6GHZ) {
        for (int ch = 1; ch <= 233 && n < cap; ch += 16)
            freqs[n++] = 5950 + ch * 5;
    }
    return n;
}
#endif /* __linux__ */

/* Advance the radio to the next channel when the dwell has elapsed. Best-effort:
 * per-channel errors (disabled / no-IR frequencies) are ignored so the sweep
 * keeps moving. */
static void wifi_hop_tick(int64_t now_us)
{
    wifi_hop_ctx *h = active_hop;
    if (h == NULL || !h->enabled || h->count == 0)
        return;
    if (h->last_hop_us != 0 && (now_us - h->last_hop_us) < (int64_t)h->dwell_ms * 1000)
        return;
    char err[128] = {0};
    (void)nero_nl80211_set_frequency(h->iface, h->freqs[h->idx], err, sizeof(err));
    h->idx = (h->idx + 1) % h->count;
    h->last_hop_us = now_us;
}

/* Inline/piped fallback: redraw the whole survey on a 1 s cadence. Used when
 * stdout is not an interactive TTY (pipe, file, CI) or on platforms without the
 * alternate-screen TUI. Output stays a plain, scriptable transcript. */
static int run_dashboard_plain(nero_wifi_session *session, int duration_seconds, int channel_filter)
{
    nero_wifi_stats previous;
    nero_wifi_session_snapshot_stats(session, &previous);
    int64_t previous_time = nero_wifi_now_us();
    int64_t started = previous_time;

    nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (aps == NULL || clients == NULL) {
        free(aps);
        free(clients);
        return -1;
    }

    for (;;) {
        if (duration_seconds > 0 && (nero_wifi_now_us() - started) / 1000000LL >= duration_seconds)
            break;
        if (cli_termination_signal != 0)
            break;
        sleep_milliseconds(1000);
        if (cli_termination_signal != 0)
            break;
        if (!nero_wifi_session_running(session))
            break;

        nero_wifi_stats current;
        nero_wifi_session_snapshot_stats(session, &current);
        int64_t now = nero_wifi_now_us();
        wifi_hop_tick(now);
        int64_t elapsed = now - previous_time;
        double pps = elapsed > 0 ? (double)(current.packets - previous.packets) * 1000000.0 /
                                       (double)elapsed
                                 : 0.0;
        if (terminal_ansi_enabled())
            clear_screen();
        int aps_count = 0, client_count = 0;
        survey_collect(session, channel_filter, 0, 0, aps, &aps_count, clients, &client_count);
        nero_table_opts opts;
        wifi_monitor_opts(&opts, nero_terminal_width());
        survey_build_frame(stdout, &opts, session, &current, aps, aps_count, clients, client_count,
                           (int)((now - started) / 1000000LL), pps, channel_filter, 0, "power", 0);
        previous = current;
        previous_time = now;
        fflush(stdout);
    }
    free(aps);
    free(clients);
    return 0;
}

#if !defined(_WIN32)
/* Flicker-free, keyboard-driven live monitor on an interactive TTY. Renders one
 * survey frame into an in-memory stream, then diffs it against the previous
 * frame so only changed rows are rewritten. Keys: s cycles AP sort, f toggles a
 * real-client filter, space pauses auto-refresh, arrows/page/home/end scroll,
 * q/Esc/Ctrl+C quit. */
static int run_dashboard_tui(nero_wifi_session *session, int duration_seconds, int channel_filter)
{
    nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (aps == NULL || clients == NULL) {
        free(aps);
        free(clients);
        return -1;
    }

    nero_operation_ui ui;
    memset(&ui, 0, sizeof(ui));
    nero_operation_begin(&ui, "WI-FI LIVE MONITOR  (read-only passive 802.11 survey)",
                         "Capture is read-only: NERO never joins, injects or changes channel.",
                         "s sort  f clients  space pause  \xE2\x86\x91\xE2\x86\x93/PgUp/PgDn/Home/End scroll  q quit");
    if (!ui.interactive) {
        nero_operation_end(&ui);
        free(aps);
        free(clients);
        return -1; /* caller falls back to the plain transcript */
    }

    int64_t started = nero_wifi_now_us();
    nero_wifi_stats previous;
    nero_wifi_session_snapshot_stats(session, &previous);
    nero_wifi_stats current = previous;
    int64_t previous_time = started;
    int64_t last_sample = 0;
    double pps = 0.0;
    int aps_count = 0, client_count = 0;
    int sort_mode = 0, clients_only = 0, paused = 0, scroll = 0, dirty = 1;

    for (;;) {
        if (cli_termination_signal != 0)
            break;
        if (duration_seconds > 0 && (nero_wifi_now_us() - started) / 1000000LL >= duration_seconds)
            break;
        if (!nero_wifi_session_running(session))
            break;

        int64_t now = nero_wifi_now_us();
        wifi_hop_tick(now);
        if (dirty || (!paused && now - last_sample >= 1000000LL)) {
            nero_wifi_session_snapshot_stats(session, &current);
            int64_t elapsed = now - previous_time;
            pps = elapsed > 0 ? (double)(current.packets - previous.packets) * 1000000.0 /
                                    (double)elapsed
                              : 0.0;
            previous = current;
            previous_time = now;
            last_sample = now;
            survey_collect(session, channel_filter, sort_mode, clients_only, aps, &aps_count,
                           clients, &client_count);
            dirty = 0;
        }

        char *buf = NULL;
        size_t buf_len = 0;
        const char *lines[600];
        int line_count = 0;
        FILE *ms = open_memstream(&buf, &buf_len);
        if (ms != NULL) {
            nero_table_opts opts;
            wifi_monitor_opts(&opts, ui.columns - 4);
            survey_build_frame(ms, &opts, session, &current, aps, aps_count, clients, client_count,
                               (int)((now - started) / 1000000LL), pps, channel_filter, paused,
                               survey_sort_label(sort_mode), clients_only);
            fclose(ms);
            char *p = buf;
            while (p != NULL && *p != '\0' &&
                   line_count < (int)(sizeof(lines) / sizeof(lines[0]))) {
                lines[line_count++] = p;
                char *nl = strchr(p, '\n');
                if (nl == NULL)
                    break;
                *nl = '\0';
                p = nl + 1;
            }
        }
        nero_operation_render_lines(&ui, lines, line_count, scroll);
        free(buf);

        int ch = 0;
        nero_ui_key key = nero_operation_read_key_ex(250, &ch);
        if (key == NERO_UI_KEY_CANCEL || key == NERO_UI_KEY_EOF)
            break;
        switch (key) {
        case NERO_UI_KEY_UP:
        case NERO_UI_KEY_WHEEL_UP:
            --scroll;
            break;
        case NERO_UI_KEY_DOWN:
        case NERO_UI_KEY_WHEEL_DOWN:
            ++scroll;
            break;
        case NERO_UI_KEY_PAGE_UP:
            scroll -= 10;
            break;
        case NERO_UI_KEY_PAGE_DOWN:
            scroll += 10;
            break;
        case NERO_UI_KEY_HOME:
            scroll = 0;
            break;
        case NERO_UI_KEY_END:
            scroll = 1 << 20;
            break;
        case NERO_UI_KEY_OTHER:
            if (ch == 's' || ch == 'S') {
                sort_mode = (sort_mode + 1) % 3;
                dirty = 1;
            } else if (ch == 'f' || ch == 'F') {
                clients_only = !clients_only;
                dirty = 1;
            } else if (ch == ' ') {
                paused = !paused;
            }
            break;
        default:
            break;
        }
        if (scroll < 0)
            scroll = 0;
    }

    nero_operation_end(&ui);
    free(aps);
    free(clients);
    return 0;
}
#endif /* !_WIN32 */

static int run_dashboard(nero_wifi_session *session, int duration_seconds, int channel_filter)
{
#if !defined(_WIN32)
    if (isatty(STDOUT_FILENO) && isatty(STDIN_FILENO) && terminal_ansi_enabled()) {
        int rc = run_dashboard_tui(session, duration_seconds, channel_filter);
        if (rc >= 0)
            return rc;
        /* Not actually interactive: fall through to the plain transcript. */
    }
#endif
    return run_dashboard_plain(session, duration_seconds, channel_filter);
}
#endif


/* The nl80211 monitor enable/disable helpers run only in a live Linux build;
 * cmd_wifi_monitor_mode (below, outside this guard) carries its own offline and
 * non-Linux stubs, so excluding these here keeps offline Linux builds free of
 * unused-function warnings. */
#if defined(__linux__) && !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
#define WIFI_MONITOR_STATE_MAGIC "nero-wifi-monitor-state=1"

static int wifi_monitor_state_path(char *path, size_t path_len)
{
    if (geteuid() == 0) {
        snprintf(path, path_len, "/run/nero-wifi-monitor.state");
        return 0;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] == '/') {
        snprintf(path, path_len, "%s/nero-wifi-monitor.state", runtime);
    } else {
        snprintf(path, path_len, "/tmp/nero-wifi-monitor-%lu.state",
                 (unsigned long)geteuid());
    }
    return 0;
}

static int wifi_monitor_save_state(const char *interface, const nero_nl80211_info *info,
                                   char *error, size_t error_len)
{
    char path[256];
    wifi_monitor_state_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(error, error_len, "could not save restore state to %s: %s", path,
                 strerror(errno));
        return -1;
    }
    (void)fchmod(fd, 0600);
    char content[256];
    int length = snprintf(content, sizeof(content), "%s\ninterface=%s\niftype=%d\n",
                          WIFI_MONITOR_STATE_MAGIC, interface, info->iftype);
    int result = 0;
    if (length <= 0 || (size_t)length >= sizeof(content)) {
        result = -1;
    } else {
        size_t written = 0;
        while (written < (size_t)length) {
            ssize_t status = write(fd, content + written, (size_t)length - written);
            if (status < 0) {
                if (errno == EINTR)
                    continue;
                result = -1;
                break;
            }
            written += (size_t)status;
        }
    }
    if (close(fd) != 0)
        result = -1;
    if (result != 0)
        snprintf(error, error_len, "could not save restore state to %s: %s", path,
                 strerror(errno));
    return result;
}

/* 1 = matching saved type, 0 = no matching state, -1 = unreadable state. */
static int wifi_monitor_load_state(const char *interface, int *iftype)
{
    char path[256];
    wifi_monitor_state_path(path, sizeof(path));
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    char buffer[256];
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0)
        return -1;
    buffer[length] = '\0';
    if (strncmp(buffer, WIFI_MONITOR_STATE_MAGIC "\n",
                strlen(WIFI_MONITOR_STATE_MAGIC) + 1) != 0)
        return -1;

    char saved_interface[NERO_NL80211_IFNAME_MAX] = {0};
    int saved_type = -1;
    char *line = strtok(buffer, "\n");
    while (line != NULL) {
        if (strncmp(line, "interface=", 10) == 0) {
            snprintf(saved_interface, sizeof(saved_interface), "%s", line + 10);
        } else if (strncmp(line, "iftype=", 7) == 0) {
            char *end = NULL;
            long value = strtol(line + 7, &end, 10);
            if (end != NULL && *end == '\0' && value >= 0 &&
                value <= NERO_NL80211_IFTYPE_NAN)
                saved_type = (int)value;
        }
        line = strtok(NULL, "\n");
    }
    if (strcmp(saved_interface, interface) != 0 || saved_type < 0 ||
        saved_type == NERO_NL80211_IFTYPE_MONITOR)
        return 0;
    *iftype = saved_type;
    return 1;
}

static void wifi_monitor_clear_state(void)
{
    char path[256];
    wifi_monitor_state_path(path, sizeof(path));
    (void)unlink(path);
}

static void wifi_monitor_drain_escape(void)
{
    char byte;
    for (int index = 0; index < 32; ++index) {
        ssize_t status = read(STDIN_FILENO, &byte, 1);
        if (status <= 0)
            return;
        if ((unsigned char)byte >= 0x40 && (unsigned char)byte <= 0x7e)
            return;
    }
}

/* The terminal session keeps stdin in raw mode, so this prompt owns echo,
 * backspace and Escape handling. Only the exact word "yes" proceeds. */
static int wifi_monitor_confirm(int enable, const char *interface)
{
    if (!isatty(STDIN_FILENO)) {
        puts("Refusing to change interface state without an interactive confirmation.");
        return 0;
    }
    printf("This changes %s to %s mode and may interrupt connectivity.\n"
           "Type 'yes' to continue, anything else cancels: ",
           interface, enable ? "monitor" : "managed");
    fflush(stdout);

    char buffer[8];
    size_t length = 0;
    int cancelled = 0;
    for (;;) {
        char byte;
        ssize_t status = read(STDIN_FILENO, &byte, 1);
        if (status <= 0) {
            cancelled = 1;
            break;
        }
        if (byte == '\n' || byte == '\r') {
            putchar('\n');
            fflush(stdout);
            break;
        }
        if (byte == 27) {
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&input, 1, 35);
            if (ready > 0 && (input.revents & POLLIN) != 0) {
                wifi_monitor_drain_escape();
                continue;
            }
            cancelled = 1;
            break;
        }
        if (byte == 'q' || byte == 'Q' || byte == 3) {
            cancelled = 1;
            break;
        }
        if (byte == 127 || byte == '\b') {
            if (length > 0) {
                --length;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if ((unsigned char)byte < 0x20 || (unsigned char)byte > 0x7e)
            continue;
        if (length + 1 >= sizeof(buffer))
            continue;
        buffer[length++] = byte;
        putchar(byte);
        fflush(stdout);
    }
    if (cancelled) {
        puts("\nCancelled; no interface state was changed.");
        return 0;
    }
    buffer[length] = '\0';
    if (strcmp(buffer, "yes") != 0) {
        puts("Confirmation was not 'yes'; no interface state was changed.");
        return 0;
    }
    return 1;
}
#endif

static cli_cmd_result cmd_wifi_monitor_mode(const char *requested, int enable)
{
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)requested;
    (void)enable;
    puts("\nwifi monitor: interface type control is unavailable in this offline build.");
    puts("No interface state was changed.");
    return CLI_CMD_CONTINUE;
#elif !defined(__linux__)
    (void)requested;
    (void)enable;
    puts("\nwifi monitor: interface type control is only available on Linux (nl80211).");
    puts("No interface state was changed.");
    return CLI_CMD_CONTINUE;
#else
    const char *interface =
        requested != NULL && requested[0] != '\0' ? requested : default_interface();
    if (interface == NULL || interface[0] == '\0') {
        puts("\nwifi monitor: no Wi-Fi interface was detected.");
        puts("No interface state was changed.");
        return CLI_CMD_CONTINUE;
    }
    nero_wifi_session *session = wifi_get_session();
    if (session != NULL && nero_wifi_session_running(session)) {
        puts("\nwifi monitor: stop the background capture before changing the interface type.");
        puts("No interface state was changed.");
        return CLI_CMD_CONTINUE;
    }

    nero_nl80211_info info;
    char error[256] = {0};
    if (nero_nl80211_get_info(interface, &info, error, sizeof(error)) != 0) {
        printf("\nwifi monitor: cannot query nl80211 for %s: %s\n", interface,
               error[0] != '\0' ? error : "unknown error");
        puts("No interface state was changed.");
        return CLI_CMD_CONTINUE;
    }

    char bands[64] = {0};
    printf("\nnl80211 interface: %s\n", interface);
    printf("  Current type:   %s\n",
           info.has_iftype ? nero_nl80211_iftype_name(info.iftype) : "unknown");
    if (info.phy_name[0] != '\0')
        printf("  Wiphy:          %s (%d channels enabled, %d disabled, %d no-IR)\n",
               info.phy_name, info.channel_count, info.disabled_channel_count,
               info.no_ir_channel_count);
    if (info.bands != 0)
        printf("  Bands:          %s\n",
               nero_nl80211_band_summary(info.bands, bands, sizeof(bands)));
    printf("  Monitor:        %s\n",
           info.has_supported_iftypes
               ? (info.monitor_supported ? "supported by this wiphy"
                                         : "not supported by this wiphy")
               : "unknown (driver did not report supported interface types)");

    if (enable) {
        if (info.has_iftype && info.iftype == NERO_NL80211_IFTYPE_MONITOR) {
            puts("\nAlready in monitor mode; no change was made.");
            return CLI_CMD_CONTINUE;
        }
        if (!info.has_supported_iftypes || !info.monitor_supported) {
            puts("\nRefusing to request monitor mode: the driver did not advertise it.");
            puts("No interface state was changed.");
            return CLI_CMD_CONTINUE;
        }
    } else if (info.has_iftype && info.iftype != NERO_NL80211_IFTYPE_MONITOR) {
        puts("\nThe interface is not in monitor mode; no change was made.");
        return CLI_CMD_CONTINUE;
    }

    if (geteuid() != 0)
        puts("\nNote: changing an interface type requires root or CAP_NET_ADMIN; the kernel "
             "may reject this request.");

    if (!wifi_monitor_confirm(enable, interface))
        return CLI_CMD_CONTINUE;

    int target = enable ? NERO_NL80211_IFTYPE_MONITOR : NERO_NL80211_IFTYPE_STATION;
    int have_saved = 0;
    int load_status = 0;
    if (!enable) {
        int saved_type = -1;
        load_status = wifi_monitor_load_state(interface, &saved_type);
        if (load_status > 0) {
            target = saved_type;
            have_saved = 1;
        }
    }

    nero_nl80211_info verified;
    error[0] = '\0';
    if (nero_nl80211_set_type(interface, target, &verified, error, sizeof(error)) != 0) {
        printf("\nwifi monitor: interface type change failed: %s\n",
               error[0] != '\0' ? error : "unknown error");
        nero_nl80211_info current;
        char current_error[256] = {0};
        if (nero_nl80211_get_info(interface, &current, current_error, sizeof(current_error)) == 0 &&
            current.has_iftype) {
            printf("Kernel now reports the interface type as: %s\n",
                   nero_nl80211_iftype_name(current.iftype));
            if (current.iftype == NERO_NL80211_IFTYPE_MONITOR)
                printf("The interface appears to be in monitor mode; run `wifi monitor disable "
                       "%s` to restore it.\n",
                       interface);
        }
        puts("No verified state change was reported. NetworkManager and service state were "
             "not modified.");
        return CLI_CMD_CONTINUE;
    }

    if (enable) {
        char state_error[192] = {0};
        if (info.has_iftype &&
            wifi_monitor_save_state(interface, &info, state_error, sizeof(state_error)) != 0)
            printf("\nWarning: %s; disable will fall back to station mode.\n", state_error);
        printf("\nMonitor mode enabled on %s (kernel reports %s).\n", interface,
               nero_nl80211_iftype_name(verified.iftype));
        printf("Restore with `wifi monitor disable %s`.\n", interface);
        puts("NetworkManager and service state were not modified.");
    } else {
        wifi_monitor_clear_state();
        printf("\nInterface %s restored to %s.\n", interface,
               nero_nl80211_iftype_name(verified.iftype));
        if (!have_saved) {
            if (load_status < 0)
                puts("Saved state was unreadable; station/managed mode was used as the fallback.");
            else
                puts("No matching saved state was found; station/managed mode was used as the "
                     "fallback.");
        }
        puts("NetworkManager and service state were not modified.");
    }
    return CLI_CMD_CONTINUE;
#endif
}

static void print_nl80211_status(const char *interface)
{
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
    puts("  nl80211:       unavailable in this offline build");
#elif defined(__linux__)
    if (interface == NULL || interface[0] == '\0') {
        puts("  nl80211:       no interface to query");
        return;
    }
    nero_nl80211_info info;
    char error[256] = {0};
    if (nero_nl80211_get_info(interface, &info, error, sizeof(error)) != 0) {
        printf("  nl80211:       unavailable (%s)\n",
               error[0] != '\0' ? error : "unknown error");
        return;
    }
    char bands[64] = {0};
    printf("  nl80211 type:  %s\n",
           info.has_iftype ? nero_nl80211_iftype_name(info.iftype) : "unknown");
    printf("  Monitor:       %s\n",
           info.has_supported_iftypes
               ? (info.monitor_supported ? "supported by this wiphy"
                                         : "not supported by this wiphy")
               : "unknown");
    if (info.bands != 0)
        printf("  Bands:         %s\n",
               nero_nl80211_band_summary(info.bands, bands, sizeof(bands)));
    printf("  Channels:      %d enabled (%d disabled, %d no-IR)\n", info.channel_count,
           info.disabled_channel_count, info.no_ir_channel_count);
#else
    (void)interface;
    puts("  nl80211:       unavailable (monitor control is Linux-only)");
#endif
}

static void print_nl80211_capabilities(const char *interface)
{
#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
    puts("  nl80211 control:  unavailable in this offline build");
#elif defined(__linux__)
    if (!nero_nl80211_available()) {
        puts("  nl80211 control:  unavailable (kernel netlink socket not usable)");
        return;
    }
    puts("  nl80211 control:  available (requires root/CAP_NET_ADMIN to change type)");
    if (interface == NULL || interface[0] == '\0') {
        puts("  Kernel monitor:   no interface to query");
        return;
    }
    nero_nl80211_info info;
    char error[256] = {0};
    if (nero_nl80211_get_info(interface, &info, error, sizeof(error)) != 0) {
        printf("  Kernel monitor:   unknown (%s)\n",
               error[0] != '\0' ? error : "unknown error");
        return;
    }
    char bands[64] = {0};
    printf("  Kernel iftype:    %s\n",
           info.has_iftype ? nero_nl80211_iftype_name(info.iftype) : "unknown");
    printf("  Kernel monitor:   %s\n",
           info.has_supported_iftypes
               ? (info.monitor_supported ? "supported by this wiphy"
                                         : "not supported by this wiphy")
               : "unknown");
    if (info.bands != 0)
        printf("  Kernel bands:     %s\n",
               nero_nl80211_band_summary(info.bands, bands, sizeof(bands)));
    printf("  Kernel channels:  %d enabled (%d disabled, %d no-IR)\n", info.channel_count,
           info.disabled_channel_count, info.no_ir_channel_count);
#else
    (void)interface;
    puts("  nl80211 control:  unavailable (Linux only)");
#endif
}

static cli_cmd_result cmd_wifi_monitor(char **tokens, size_t count)
{
    const char *action = "foreground";
    const char *interface = NULL;
    int action_seen = 0;
    int duration = 0;
    int channel = -1;
    int request_link = 0;
    int want_hop = 0;
    int dwell_ms = 300;
    unsigned hop_bands = 0;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--duration") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 1, 86400, &duration) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--link") == 0) {
            request_link = 1;
        } else if (strcmp(tokens[i], "--hop") == 0) {
            want_hop = 1;
        } else if (strcmp(tokens[i], "--dwell") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 50, 10000, &dwell_ms) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--bands") == 0 && i + 1 < count) {
            hop_bands = wifi_parse_hop_bands(tokens[++i]);
            if (hop_bands == 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "start") == 0 || strcmp(tokens[i], "stop") == 0 ||
                   strcmp(tokens[i], "status") == 0 || strcmp(tokens[i], "enable") == 0 ||
                   strcmp(tokens[i], "disable") == 0) {
            if (action_seen)
                return CLI_CMD_INVALID;
            action = tokens[i];
            action_seen = 1;
        } else if (tokens[i][0] == '-') {
            return CLI_CMD_INVALID;
        } else if (interface == NULL) {
            interface = tokens[i];
        } else {
            return CLI_CMD_INVALID;
        }
    }

    /* Channel hopping only applies to the foreground live survey. */
    if (want_hop && action_seen)
        return CLI_CMD_INVALID;

    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;

    if (strcmp(action, "stop") == 0) {
        if (!nero_wifi_session_running(session)) {
            puts("\nNo background Wi-Fi capture is running.");
            return CLI_CMD_CONTINUE;
        }
        nero_wifi_session_stop(session);
        nero_wifi_stats stats;
        nero_wifi_session_snapshot_stats(session, &stats);
        puts("\nWi-Fi capture stopped. The interface was released; its power/profile was not "
             "modified.");
        print_stats_summary(&stats);
        return CLI_CMD_CONTINUE;
    }

    if (strcmp(action, "status") == 0) {
        nero_wifi_stats stats;
        char interface[NERO_WIFI_IFNAME_MAX] = {0};
        char last_error[256] = {0};
        nero_wifi_session_snapshot_stats(session, &stats);
        nero_wifi_session_interface(session, interface, sizeof(interface));
        printf("\nWi-Fi capture: %s\n", nero_wifi_session_running(session) ? "RUNNING" : "idle");
        printf("Interface:     %s\n", interface[0] != '\0' ? interface : "(none)");
        print_stats_summary(&stats);
        if (nero_wifi_session_last_error(session, last_error, sizeof(last_error)) > 0) {
            printf("Last error:    %s\n", last_error);
        }
        print_nl80211_status(interface[0] != '\0' && strcmp(interface, "file") != 0 ? interface
                                                                                     : NULL);
        return CLI_CMD_CONTINUE;
    }

    if (strcmp(action, "enable") == 0 || strcmp(action, "disable") == 0) {
        if (duration != 0 || channel >= 0 || request_link || want_hop)
            return CLI_CMD_INVALID;
        return cmd_wifi_monitor_mode(interface, strcmp(action, "enable") == 0);
    }

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
    (void)duration;
    (void)channel;
    (void)request_link;
    (void)want_hop;
    (void)dwell_ms;
    (void)hop_bands;
    puts("\nwifi monitor: live capture is unavailable in this offline build.");
    puts("Use `wifi analyze <PCAP_FILE>` to inspect a genuine capture file.");
    return CLI_CMD_CONTINUE;
#else
    int was_running = nero_wifi_session_running(session);
    if (!was_running) {
        const char *target = interface != NULL ? interface : default_interface();
        if (target == NULL || target[0] == '\0') {
            puts("\nwifi monitor: no Wi-Fi interface was detected.");
            return CLI_CMD_CONTINUE;
        }
        char error[256] = {0};
        int request_monitor = request_link ? 0 : 1;
#if defined(__linux__)
        nero_nl80211_info nl_info;
        char nl_error[256] = {0};
        if (request_monitor && nero_nl80211_get_info(target, &nl_info, nl_error,
                                                    sizeof(nl_error)) == 0 &&
            nl_info.has_iftype && nl_info.iftype == NERO_NL80211_IFTYPE_MONITOR)
            request_monitor = 0;
#endif
        if (nero_wifi_session_start(session, target, request_monitor, error, sizeof(error)) != 0) {
            printf("\nwifi monitor: could not start passive capture: %s\n",
                   error[0] != '\0' ? error : "unknown error");
            puts("No capture data was fabricated. Possible causes:");
            puts("  - built-in Apple Silicon Wi-Fi does not expose 802.11 monitor capture");
            puts("  - BPF device permission denied");
            puts("  - libpcap/Npcap missing or too old");
            puts("Import a genuine monitor-mode pcap with `wifi analyze <PCAP_FILE>` instead.");
            return CLI_CMD_CONTINUE;
        }
    }

    if (strcmp(action, "start") == 0) {
        char interface[NERO_WIFI_IFNAME_MAX] = {0};
        nero_wifi_session_interface(session, interface, sizeof(interface));
        printf("\n%s capture started on %s. Use `wifi monitor stop` to stop it.\n",
               request_link ? "Link-layer" : "Passive monitor",
               interface[0] != '\0' ? interface : "(unknown interface)");
        return CLI_CMD_CONTINUE;
    }

    wifi_hop_ctx hop;
    memset(&hop, 0, sizeof(hop));
    if (want_hop) {
        if (hop_bands == 0)
            hop_bands = WIFI_HOP_2GHZ;
        char hop_iface[NERO_WIFI_IFNAME_MAX] = {0};
        nero_wifi_session_interface(session, hop_iface, sizeof(hop_iface));
#if defined(__linux__)
        char nl_hop_error[256] = {0};
        nero_nl80211_info hop_info;
        if (geteuid() != 0) {
            puts("\nwifi monitor --hop: channel hopping needs root/CAP_NET_ADMIN; running a "
                 "single-channel survey instead.");
        } else if (nero_nl80211_get_info(hop_iface, &hop_info, nl_hop_error,
                                         sizeof(nl_hop_error)) != 0 ||
                   !hop_info.has_iftype || hop_info.iftype != NERO_NL80211_IFTYPE_MONITOR) {
            puts("\nwifi monitor --hop: the interface is not in monitor mode. Run `wifi monitor "
                 "enable` first; running a single-channel survey instead.");
        } else {
            puts("\nChannel hopping repeatedly changes the adapter channel (read-only capture, no "
                 "injection).");
            if (!wifi_monitor_confirm(1, hop_iface)) {
                puts("Channel hopping was not confirmed; running a single-channel survey instead.");
            } else {
                hop.count = wifi_build_freq_plan(hop_bands, hop.freqs,
                                                 (int)(sizeof(hop.freqs) / sizeof(hop.freqs[0])));
                hop.dwell_ms = dwell_ms;
                snprintf(hop.iface, sizeof(hop.iface), "%s", hop_iface);
                hop.enabled = hop.count > 0;
                if (hop.enabled)
                    printf("Channel hopping enabled: %d channel(s), %d ms dwell.\n", hop.count,
                           hop.dwell_ms);
            }
        }
#else
        (void)dwell_ms;
        (void)hop_bands;
        puts("\nwifi monitor --hop: in-NERO channel control is only available on Linux (nl80211). "
             "On this platform the survey stays on the adapter's current channel.");
#endif
    }
    active_hop = hop.enabled ? &hop : NULL;

    puts("\nStarting passive monitor. Press Ctrl+C to stop.");
    fflush(stdout);
    run_dashboard(session, duration, channel);
    active_hop = NULL;
    if (nero_wifi_session_running(session))
        nero_wifi_session_stop(session);
    if (cli_termination_signal == SIGINT)
        cli_termination_signal = 0;
    nero_wifi_stats stats;
    nero_wifi_session_snapshot_stats(session, &stats);
    puts("\nCapture stopped.");
    print_stats_summary(&stats);
    char last_error[256] = {0};
    if (nero_wifi_session_last_error(session, last_error, sizeof(last_error)) > 0) {
        printf("Last error:    %s\n", last_error);
    }
    nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (aps != NULL && clients != NULL) {
        int aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, -1);
        int client_count =
            nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, -1);
        print_ap_table(aps, aps_count);
        print_client_table(clients, client_count, 0);
    }
    free(aps);
    free(clients);
    return CLI_CMD_CONTINUE;
#endif
}

cli_cmd_result cmd_wifi(char **tokens, size_t count)
{
    if (count == 1 || (count == 2 && strcmp(tokens[1], "help") == 0)) {
        print_wifi_help();
        return CLI_CMD_CONTINUE;
    }
    const char *command = tokens[1];
    if (strcmp(command, "status") == 0) {
        if (count != 2)
            return CLI_CMD_INVALID;
        return cmd_wifi_status();
    }
    if (strcmp(command, "capabilities") == 0) {
        if (count != 2)
            return CLI_CMD_INVALID;
        return cmd_wifi_capabilities();
    }
    if (strcmp(command, "interfaces") == 0) {
        if (count != 2)
            return CLI_CMD_INVALID;
        return cmd_wifi_interfaces();
    }
    if (strcmp(command, "scan") == 0)
        return cmd_wifi_scan(tokens + 1, count - 1);
    if (strcmp(command, "aps") == 0)
        return cmd_wifi_aps(tokens + 1, count - 1);
    if (strcmp(command, "clients") == 0)
        return cmd_wifi_clients(tokens + 1, count - 1);
    if (strcmp(command, "signal") == 0)
        return cmd_wifi_signal(tokens + 1, count - 1);
    if (strcmp(command, "inspect") == 0)
        return cmd_wifi_inspect(tokens + 1, count - 1);
    if (strcmp(command, "analyze") == 0)
        return cmd_wifi_analyze(tokens + 1, count - 1);
    if (strcmp(command, "export") == 0)
        return cmd_wifi_export(tokens + 1, count - 1);
    if (strcmp(command, "monitor") == 0)
        return cmd_wifi_monitor(tokens + 1, count - 1);
    if (strcmp(command, "clear") == 0) {
        if (count != 2)
            return CLI_CMD_INVALID;
        nero_wifi_session *session = wifi_get_session();
        int was_running = nero_wifi_session_running(session);
        if (session != NULL)
            nero_wifi_session_reset(session);
        puts("\nWi-Fi capture data cleared from memory.");
        if (was_running)
            puts("The active capture was stopped.");
        return CLI_CMD_CONTINUE;
    }
    return CLI_CMD_INVALID;
}
