/* Wi-Fi command surface and terminal dashboards. */

#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#endif
#include "cli_internal.h"
#include "nero/terminal.h"
#include "nero/wifi.h"
#include "nero/wifi_capture.h"
#include "nero/wifi_platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIFI_IDLE_SECONDS 60

static nero_wifi_session *wifi_session;
static int wifi_atexit_registered;

static uint64_t anonymization_salt(void);
static void anonymize_mac_address(const uint8_t input[NERO_WIFI_MAC_LEN],
                                  uint8_t output[NERO_WIFI_MAC_LEN]);

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
         "  wifi interfaces                     List Wi-Fi interfaces\n"
         "  wifi scan [INTERFACE]               Active CoreWLAN survey (never joins a network)\n"
         "  wifi aps [--channel N]              Show access points from passive data\n"
         "  wifi clients [--channel N] [--anonymize]\n"
         "                                      Show clients grouped by access point\n"
         "  wifi inspect <MAC|BSSID>            Inspect a client or access point\n"
         "  wifi monitor [INTERFACE]            Live passive dashboard (Ctrl+C stops)\n"
         "  wifi monitor start [INTERFACE]      Start a background passive session\n"
         "  wifi monitor stop                   Stop the background session\n"
         "  wifi monitor status                 Show session counters\n"
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
 *  Innovative terminal table rendering.
 *
 *  Box-drawing and colour are capability-gated: NO_COLOR and non-ANSI or
 *  non-UTF-8 terminals automatically fall back to a plain ASCII grid, so the
 *  output stays copy-pasteable and scriptable. Colours encode meaning only
 *  (signal strength, security posture, radio band); they never invent data.
 * ------------------------------------------------------------------------- */

#define WIFI_RESET "\033[0m"
#define WIFI_DIM   "\033[2m"
#define WIFI_HEAD  "\033[1;36m"
#define WIFI_GREEN "\033[32m"
#define WIFI_YEL   "\033[33m"
#define WIFI_RED   "\033[31m"
#define WIFI_CYAN  "\033[36m"
#define WIFI_MAG   "\033[35m"

typedef struct {
    const char *h, *v, *tl, *tr, *bl, *br, *lt, *rt, *tt, *bt, *x;
} wifi_box;

static int wifi_color(void) { return terminal_color_enabled(); }
static int wifi_unicode(void) { return terminal_unicode_enabled(); }

/* Returns the colour escape when colour is enabled, else an empty string. */
static const char *wc(const char *code) { return wifi_color() ? code : ""; }
static const char *wc_off(void) { return wifi_color() ? WIFI_RESET : ""; }

static wifi_box wifi_box_glyphs(void)
{
    if (wifi_unicode()) {
        wifi_box unicode = {"─", "│", "┌", "┐", "└", "┘",
                            "├", "┤", "┬", "┴", "┼"};
        return unicode;
    }
    wifi_box ascii = {"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
    return ascii;
}

/* Horizontal border: left/mid/right choose the junction glyphs so one helper
 * draws the top (tl/tt/tr), divider (lt/x/rt) and bottom (bl/bt/br) rules. */
static void wifi_rule(const wifi_box *b, const char *left, const char *mid, const char *right,
                      const int *widths, int columns)
{
    fputs(wc(WIFI_DIM), stdout);
    fputs(left, stdout);
    for (int i = 0; i < columns; ++i) {
        for (int k = 0; k < widths[i] + 2; ++k)
            fputs(b->h, stdout);
        fputs(i + 1 < columns ? mid : right, stdout);
    }
    fputs(wc_off(), stdout);
    putchar('\n');
}

static void wifi_vbar(const wifi_box *b)
{
    fputs(wc(WIFI_DIM), stdout);
    fputs(b->v, stdout);
    fputs(wc_off(), stdout);
}

/* One padded cell. `text` is truncated/padded to the visible width, then the
 * colour (if any) wraps the already-sized text so alignment never drifts. */
static void wifi_cell(const wifi_box *b, const char *text, int width, int right, const char *color)
{
    wifi_vbar(b);
    putchar(' ');
    if (color)
        fputs(wc(color), stdout);
    if (right)
        printf("%*.*s", width, width, text);
    else
        printf("%-*.*s", width, width, text);
    if (color)
        fputs(wc_off(), stdout);
    putchar(' ');
}

static void wifi_row_end(const wifi_box *b)
{
    wifi_vbar(b);
    putchar('\n');
}

static void wifi_header_row(const wifi_box *b, const char *const *titles, const int *widths,
                            const int *right, int columns)
{
    for (int i = 0; i < columns; ++i)
        wifi_cell(b, titles[i], widths[i], right ? right[i] : 0, WIFI_HEAD);
    wifi_row_end(b);
}

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

/* Maps an RSSI to a 1..4 strength level and a traffic-light colour. Level 0
 * means "no signal information was captured" and is never shown as weak. */
static int rssi_level(int has_rssi, int rssi, const char **color)
{
    if (!has_rssi) {
        *color = WIFI_DIM;
        return 0;
    }
    if (rssi >= -55) {
        *color = WIFI_GREEN;
        return 4;
    }
    if (rssi >= -67) {
        *color = WIFI_GREEN;
        return 3;
    }
    if (rssi >= -75) {
        *color = WIFI_YEL;
        return 2;
    }
    *color = WIFI_RED;
    return 1;
}

/* Signal column: a four-segment meter plus the raw dBm, total visible width 9
 * so it aligns like any other fixed cell. */
static void wifi_signal_cell(const wifi_box *b, int has_rssi, int rssi)
{
    static const char *bars_u[4] = {"▂", "▄", "▆", "█"};
    const char *color = WIFI_DIM;
    int level = rssi_level(has_rssi, rssi, &color);
    char dbm[8];
    if (has_rssi)
        snprintf(dbm, sizeof(dbm), "%d", rssi);
    else
        snprintf(dbm, sizeof(dbm), "--");

    wifi_vbar(b);
    putchar(' ');
    for (int i = 0; i < 4; ++i) {
        int on = i < level;
        fputs(wc(on ? color : WIFI_DIM), stdout);
        if (wifi_unicode())
            fputs(on ? bars_u[i] : "▁", stdout);
        else
            putchar(on ? '#' : '.');
        fputs(wc_off(), stdout);
    }
    printf(" %s%4s%s ", wc(color), dbm, wc_off());
}

static void print_ap_table(const nero_wifi_ap *aps, int count)
{
    printf("\n%sACCESS POINTS%s  %s(%d observed)%s\n", wc(WIFI_HEAD), wc_off(), wc(WIFI_DIM), count,
           wc_off());
    if (count == 0) {
        printf("  %s(no access points in the current data set)%s\n", wc(WIFI_DIM), wc_off());
        return;
    }
    const wifi_box b = wifi_box_glyphs();
    enum { COLS = 8 };
    const int widths[COLS] = {9, 22, 17, 3, 7, 8, 7, 19};
    const int right[COLS] = {0, 0, 0, 1, 0, 0, 1, 0};
    const char *const titles[COLS] = {"SIGNAL",  "SSID",     "BSSID",   "CH",
                                      "BAND",    "SECURITY", "CLIENTS", "LAST SEEN"};
    wifi_rule(&b, b.tl, b.tt, b.tr, widths, COLS);
    wifi_header_row(&b, titles, widths, right, COLS);
    wifi_rule(&b, b.lt, b.x, b.rt, widths, COLS);
    for (int i = 0; i < count; ++i) {
        char ssid[64];
        char bssid[32];
        char channel[8];
        char last[32];
        char clients[8];
        int hidden = aps[i].hidden || aps[i].redacted || aps[i].ssid_len == 0;
        if (aps[i].has_channel)
            snprintf(channel, sizeof(channel), "%d", aps[i].channel);
        else
            snprintf(channel, sizeof(channel), "-");
        snprintf(clients, sizeof(clients), "%d", aps[i].client_count);
        nero_wifi_format_time(aps[i].last_seen_us, last, sizeof(last));

        wifi_signal_cell(&b, aps[i].has_rssi, aps[i].rssi);
        wifi_cell(&b, display_ssid(&aps[i], ssid, sizeof(ssid)), widths[1], 0,
                  hidden ? WIFI_DIM : NULL);
        wifi_cell(&b, display_bssid(&aps[i], bssid, sizeof(bssid)), widths[2], 0, NULL);
        wifi_cell(&b, channel, widths[3], 1, NULL);
        wifi_cell(&b, aps[i].band == NERO_WIFI_BAND_UNKNOWN ? "-" : nero_wifi_band_name(aps[i].band),
                  widths[4], 0, band_color(aps[i].band));
        wifi_cell(&b, nero_wifi_security_name(aps[i].security), widths[5], 0,
                  security_color(aps[i].security));
        wifi_cell(&b, clients, widths[6], 1, NULL);
        wifi_cell(&b, last, widths[7], 0, WIFI_DIM);
        wifi_row_end(&b);
    }
    wifi_rule(&b, b.bl, b.bt, b.br, widths, COLS);
}

static const char *confidence_color(nero_wifi_confidence confidence)
{
    switch (confidence) {
    case NERO_WIFI_CONFIDENCE_ASSOCIATED: return WIFI_GREEN;
    case NERO_WIFI_CONFIDENCE_LIKELY: return WIFI_YEL;
    default: return WIFI_DIM;
    }
}

static void print_client_rows(const nero_wifi_client *clients, int count, int anonymize,
                              const uint8_t *only_bssid)
{
    int64_t now = nero_wifi_now_us();
    char current_ssid[NERO_WIFI_SSID_CAP] = {0};
    uint8_t current_bssid[NERO_WIFI_MAC_LEN] = {0};
    int have_group = 0;
    int printed = 0;
    const wifi_box b = wifi_box_glyphs();
    enum { COLS = 5 };
    const int widths[COLS] = {17, 9, 11, 6, 18};
    const int right[COLS] = {0, 0, 0, 1, 0};
    const char *const titles[COLS] = {"DEVICE MAC", "SIGNAL", "STATUS", "AGE", "NOTES"};

    for (int i = 0; i < count; ++i) {
        if (only_bssid != NULL && (!clients[i].has_bssid ||
                                   memcmp(clients[i].bssid, only_bssid, NERO_WIFI_MAC_LEN) != 0)) {
            continue;
        }
        int new_group = !have_group ||
                        memcmp(current_bssid, clients[i].bssid, NERO_WIFI_MAC_LEN) != 0 ||
                        strcmp(current_ssid, clients[i].ssid) != 0;
        if (new_group) {
            char bssid[32];
            char label[64];
            if (have_group)
                wifi_rule(&b, b.bl, b.bt, b.br, widths, COLS);
            if (clients[i].has_bssid)
                nero_wifi_format_mac(clients[i].bssid, bssid, sizeof(bssid));
            else
                snprintf(bssid, sizeof(bssid), "-");
            if (anonymize && clients[i].has_bssid) {
                uint8_t clean[NERO_WIFI_MAC_LEN];
                anonymize_mac_address(clients[i].bssid, clean);
                nero_wifi_format_mac(clean, bssid, sizeof(bssid));
            }
            if (clients[i].ssid[0] != '\0')
                snprintf(label, sizeof(label), "%s", clients[i].ssid);
            else
                snprintf(label, sizeof(label), "<hidden/unknown>");
            printf("\n  %s%s%s %s%s%s %s(%s)%s\n", wc(WIFI_CYAN), wifi_unicode() ? "▸" : ">",
                   wc_off(), wc(WIFI_HEAD), label, wc_off(), wc(WIFI_DIM), bssid, wc_off());
            wifi_rule(&b, b.tl, b.tt, b.tr, widths, COLS);
            wifi_header_row(&b, titles, widths, right, COLS);
            wifi_rule(&b, b.lt, b.x, b.rt, widths, COLS);
            memcpy(current_bssid, clients[i].bssid, NERO_WIFI_MAC_LEN);
            memcpy(current_ssid, clients[i].ssid, NERO_WIFI_SSID_CAP);
            have_group = 1;
        }
        char mac[32];
        char age[24];
        nero_wifi_format_mac(clients[i].mac, mac, sizeof(mac));
        if (anonymize) {
            uint8_t clean[NERO_WIFI_MAC_LEN];
            anonymize_mac_address(clients[i].mac, clean);
            nero_wifi_format_mac(clean, mac, sizeof(mac));
        }
        format_age(&clients[i], now, age, sizeof(age));
        char notes[64] = "";
        if (clients[i].state == NERO_WIFI_STATE_DISASSOCIATED)
            strncat(notes, "LEFT ", sizeof(notes) - strlen(notes) - 1);
        if (clients[i].randomized)
            strncat(notes, "RANDOM-MAC ", sizeof(notes) - strlen(notes) - 1);
        if ((now - clients[i].last_seen_us) > (int64_t)WIFI_IDLE_SECONDS * 1000000LL)
            strncat(notes, "IDLE ", sizeof(notes) - strlen(notes) - 1);

        wifi_cell(&b, mac, widths[0], 0, clients[i].randomized ? WIFI_DIM : NULL);
        wifi_signal_cell(&b, clients[i].has_rssi, clients[i].rssi);
        wifi_cell(&b, nero_wifi_confidence_name(clients[i].confidence), widths[2], 0,
                  confidence_color(clients[i].confidence));
        wifi_cell(&b, age, widths[3], 1, WIFI_DIM);
        wifi_cell(&b, notes, widths[4], 0, notes[0] ? WIFI_YEL : NULL);
        wifi_row_end(&b);
        ++printed;
    }
    if (printed == 0)
        printf("  %s(no client devices in the current data set)%s\n", wc(WIFI_DIM), wc_off());
    else
        wifi_rule(&b, b.bl, b.bt, b.br, widths, COLS);
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
    for (int i = 0; i < 256; ++i) {
        if (channels[i] <= 0)
            continue;
        int bars = (channels[i] * 20 + max - 1) / max;
        const char *color = i <= 14 ? WIFI_YEL : (i < 100 ? WIFI_CYAN : WIFI_MAG);
        printf("  %sch%3d%s %s", wc(WIFI_DIM), i, wc_off(), wc(color));
        for (int k = 0; k < bars; ++k)
            fputs(block, stdout);
        printf("%s %s%d AP%s%s\n", wc_off(), wc(WIFI_DIM), channels[i], channels[i] == 1 ? "" : "s",
               wc_off());
    }
}

static void stats_kv(const wifi_box *b, const char *label, const char *value, int lw, int vw,
                     const char *vcolor)
{
    wifi_cell(b, label, lw, 0, WIFI_DIM);
    wifi_cell(b, value, vw, 0, vcolor);
    wifi_row_end(b);
}

static void print_stats_summary(const nero_wifi_stats *stats)
{
    const wifi_box b = wifi_box_glyphs();
    enum { LW = 12, VW = 42 };
    const int widths[2] = {LW, VW};
    char buf[96];

    printf("\n%sCAPTURE STATISTICS%s\n", wc(WIFI_HEAD), wc_off());
    wifi_rule(&b, b.tl, b.tt, b.tr, widths, 2);
    stats_kv(&b, "Source", stats->source[0] != '\0' ? stats->source : "(no capture source)", LW, VW,
             NULL);
    snprintf(buf, sizeof(buf), "%d", stats->linktype);
    stats_kv(&b, "Link type", buf, LW, VW, NULL);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->packets);
    stats_kv(&b, "Packets", buf, LW, VW, WIFI_CYAN);
    snprintf(buf, sizeof(buf), "%llu  (mgmt %llu, data %llu, ctrl %llu)",
             (unsigned long long)stats->frames, (unsigned long long)stats->management,
             (unsigned long long)stats->data, (unsigned long long)stats->control);
    stats_kv(&b, "Frames", buf, LW, VW, NULL);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->bytes);
    stats_kv(&b, "Bytes", buf, LW, VW, NULL);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->malformed);
    stats_kv(&b, "Malformed", buf, LW, VW, stats->malformed ? WIFI_YEL : WIFI_DIM);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->duplicates);
    stats_kv(&b, "Duplicates", buf, LW, VW, WIFI_DIM);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->non_wifi);
    stats_kv(&b, "Non-802.11", buf, LW, VW, WIFI_DIM);
    snprintf(buf, sizeof(buf), "%llu  (capture + table pressure)",
             (unsigned long long)stats->dropped);
    stats_kv(&b, "Dropped", buf, LW, VW, stats->dropped ? WIFI_YEL : WIFI_DIM);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats->evicted);
    stats_kv(&b, "Evicted", buf, LW, VW, WIFI_DIM);
    wifi_rule(&b, b.bl, b.bt, b.br, widths, 2);
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
    const wifi_box b = wifi_box_glyphs();
    enum { COLS = 6 };
    const int widths[COLS] = {14, 18, 7, 8, 7, 14};
    const int right[COLS] = {0, 0, 0, 0, 1, 0};
    const char *const titles[COLS] = {"NAME",    "HARDWARE ADDRESS", "POWER",
                                      "SERVICE", "CHANNEL",          "MODE"};
    wifi_rule(&b, b.tl, b.tt, b.tr, widths, COLS);
    wifi_header_row(&b, titles, widths, right, COLS);
    wifi_rule(&b, b.lt, b.x, b.rt, widths, COLS);
    for (int i = 0; i < count; ++i) {
        char channel[16];
        char mode[16];
        if (interfaces[i].has_channel)
            snprintf(channel, sizeof(channel), "%d", interfaces[i].channel);
        else
            snprintf(channel, sizeof(channel), "-");
        snprintf(mode, sizeof(mode), "%d", interfaces[i].mode);
        wifi_cell(&b, interfaces[i].name, widths[0], 0, NULL);
        wifi_cell(&b,
                  interfaces[i].hardware_address[0] != '\0' ? interfaces[i].hardware_address
                                                            : "unknown",
                  widths[1], 0, NULL);
        wifi_cell(&b, interfaces[i].powered ? "on" : "off", widths[2], 0,
                  interfaces[i].powered ? WIFI_GREEN : WIFI_DIM);
        wifi_cell(&b, interfaces[i].service_active ? "active" : "inactive", widths[3], 0,
                  interfaces[i].service_active ? WIFI_GREEN : WIFI_DIM);
        wifi_cell(&b, channel, widths[4], 1, NULL);
        wifi_cell(&b, mode, widths[5], 0, WIFI_DIM);
        wifi_row_end(&b);
    }
    wifi_rule(&b, b.bl, b.bt, b.br, widths, COLS);
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
    if (count == 2)
        interface = tokens[1];
    else if (count > 2)
        return CLI_CMD_INVALID;

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
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
    int count_aps = 0;
    nero_wifi_platform_capabilities capabilities;
    char error[256] = {0};
    if (nero_wifi_platform_scan(interface, aps, 256, &count_aps, &capabilities, error,
                                sizeof(error)) != 0) {
        printf("\nwifi scan failed: %s\n", error[0] != '\0' ? error : "unknown error");
        free(aps);
        return CLI_CMD_CONTINUE;
    }
    qsort(aps, (size_t)count_aps, sizeof(aps[0]), ap_sort_compare);
    puts("\nACTIVE WIRELESS SURVEY (CoreWLAN; no network was joined)");
    if (capabilities.identifiers_redacted) {
        puts(
            "  NOTE: macOS redacted one or more SSID/BSSID values. Grant Location Services to the");
        puts("        terminal application to reveal identifiers. Hidden values are never "
             "invented.");
    }
    print_ap_table(aps, count_aps);
    printf("\n  %d access point(s) reported by the active scan.\n", count_aps);
    puts("  Active survey results are separate from passive session data; use `wifi analyze`");
    puts("  or `wifi monitor` to populate data shown by `wifi aps` and `wifi export`.");
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
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--anonymize") == 0) {
            anonymize = 1;
        } else {
            return CLI_CMD_INVALID;
        }
    }
    nero_wifi_session *session = wifi_get_session();
    if (session == NULL)
        return CLI_CMD_FAILURE;
    nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
    if (clients == NULL)
        return CLI_CMD_FAILURE;
    int client_count =
        nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS, channel);
    print_client_table(clients, client_count, anonymize);
    printf("\n  %d client device(s) observed. Observation is not proof of a live connection.\n",
           client_count);
    if (channel >= 0)
        printf("  Channel filter: %d\n", channel);
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
    FILE *file = fopen(path, "w");
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

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
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
#endif

#if !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
static void print_dashboard_header(nero_wifi_session *session, const nero_wifi_stats *stats,
                                   double pps, int channel_filter, int ap_count, int client_count)
{
    char channel[32];
    char interface[NERO_WIFI_IFNAME_MAX] = {0};
    nero_wifi_session_interface(session, interface, sizeof(interface));
    if (channel_filter >= 0)
        snprintf(channel, sizeof(channel), "%d (filtered)", channel_filter);
    else
        snprintf(channel, sizeof(channel), "current monitor channel");
    printf("\nNERO - WIRELESS MONITOR\n"
           "Interface: %s\n"
           "Capture:   %s\n"
           "Backend:   %s\n"
           "Mode:      PASSIVE%s\n"
           "Channel:   %s\n"
           "Packets:   %llu  (%.1f pkt/s)   Dropped: %llu\n"
           "Frames:    %llu  APs: %d  Clients: %d\n",
           interface[0] != '\0' ? interface : "(unknown)",
           nero_wifi_session_running(session) ? "ACTIVE" : "STOPPED",
           stats->backend[0] != '\0' ? stats->backend : "libpcap/Npcap",
           stats->monitor_requested ? "" : " (monitor not requested)", channel,
           (unsigned long long)stats->packets, pps, (unsigned long long)stats->dropped,
           (unsigned long long)stats->frames, ap_count, client_count);
    if (stats->malformed > 0)
        printf("Malformed: %llu\n", (unsigned long long)stats->malformed);
    if (stats->non_wifi > 0)
        printf("Non-802.11 packets: %llu\n", (unsigned long long)stats->non_wifi);
}

static int run_dashboard(nero_wifi_session *session, int duration_seconds, int channel_filter)
{
    nero_wifi_stats previous;
    nero_wifi_session_snapshot_stats(session, &previous);
    int64_t previous_time = nero_wifi_now_us();
    int64_t started = previous_time;
    int zero_warning = 0;

    for (;;) {
        if (duration_seconds > 0 &&
            (nero_wifi_now_us() - started) / 1000000LL >= duration_seconds) {
            break;
        }
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
        int64_t elapsed = now - previous_time;
        double pps =
            elapsed > 0 ? (double)(current.packets - previous.packets) * 1000000.0 / (double)elapsed
                        : 0.0;
        if (terminal_ansi_enabled())
            clear_screen();
        nero_wifi_ap *aps = calloc(NERO_WIFI_MAX_APS, sizeof(*aps));
        nero_wifi_client *clients = calloc(NERO_WIFI_MAX_CLIENTS, sizeof(*clients));
        int aps_count = 0;
        int client_count = 0;
        if (aps != NULL && clients != NULL) {
            aps_count = nero_wifi_session_get_aps(session, aps, NERO_WIFI_MAX_APS, channel_filter);
            client_count = nero_wifi_session_get_clients(session, clients, NERO_WIFI_MAX_CLIENTS,
                                                         channel_filter);
        }
        print_dashboard_header(session, &current, pps, channel_filter, aps_count, client_count);
        if (aps != NULL && clients != NULL) {
            print_ap_table(aps, aps_count);
            print_client_table(clients, client_count, 0);
        }
        free(aps);
        free(clients);

        if (!zero_warning && current.packets == 0 && (now - started) > 3000000LL) {
            zero_warning = 1;
            puts("\nWARNING: monitor mode was accepted but zero packets were delivered.");
            puts("On Apple Silicon the integrated Wi-Fi adapter may not expose 802.11 monitor");
            puts("capture. Use a genuine monitor-mode pcap with `wifi analyze` if so.");
        }
        if (current.dropped > 0) {
            printf("\nCapture loss: %llu packet(s) dropped by the kernel/backend.\n",
                   (unsigned long long)current.dropped);
        }
        previous = current;
        previous_time = now;
        fflush(stdout);
    }
    return 0;
}
#endif

static cli_cmd_result cmd_wifi_monitor(char **tokens, size_t count)
{
    const char *action = "foreground";
    const char *interface = NULL;
    int action_seen = 0;
    int duration = 0;
    int channel = -1;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(tokens[i], "--duration") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 1, 86400, &duration) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "--channel") == 0 && i + 1 < count) {
            if (parse_number(tokens[++i], 0, 233, &channel) != 0)
                return CLI_CMD_INVALID;
        } else if (strcmp(tokens[i], "start") == 0 || strcmp(tokens[i], "stop") == 0 ||
                   strcmp(tokens[i], "status") == 0) {
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
        return CLI_CMD_CONTINUE;
    }

#if defined(NERO_NETWORK_UNAVAILABLE) || defined(NERO_SAFE_TEST)
    (void)interface;
    (void)duration;
    (void)channel;
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
        if (nero_wifi_session_start(session, target, 1, error, sizeof(error)) != 0) {
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
        printf("\nPassive capture started on %s. Use `wifi monitor stop` to stop it.\n",
               interface[0] != '\0' ? interface : "(unknown interface)");
        return CLI_CMD_CONTINUE;
    }

    puts("\nStarting passive monitor. Press Ctrl+C to stop.");
    fflush(stdout);
    run_dashboard(session, duration, channel);
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
