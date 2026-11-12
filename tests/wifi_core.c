/* Deterministic offline tests for the 802.11 parser, tracker and pcap reader. */

#include "../src/modules/wifi/wifi_internal.h"
#include "nero/wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static const uint8_t BSSID[NERO_WIFI_MAC_LEN] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t CLIENT_MAC[NERO_WIFI_MAC_LEN] = {0xda, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5};
static const uint8_t DEST_MAC[NERO_WIFI_MAC_LEN] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
static const uint8_t BROADCAST_MAC[NERO_WIFI_MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)(value >> 24);
}

static size_t build_radiotap(uint8_t *out, int frequency, int rssi)
{
    memset(out, 0, 16);
    out[0] = 0; /* radiotap version */
    put_le16(out + 2, 16);
    put_le32(out + 4, 0x0000002au); /* flags + channel + dBm antenna signal */
    out[8] = 0;                     /* flags: no FCS */
    put_le16(out + 10, (uint16_t)frequency);
    put_le16(out + 12, 0);
    out[14] = (uint8_t)(int8_t)rssi;
    return 16;
}

static size_t build_radiotap_extended(uint8_t *out, int frequency, int rssi)
{
    size_t header = build_radiotap(out, frequency, rssi);
    put_le32(out + 4, 0x1000002au); /* flags + channel + dBm + extended field 28 */
    put_le16(out + 2, 28);
    memset(out + header, 0, 12);
    return 28;
}

static size_t append_ssid(uint8_t *out, const char *ssid)
{
    size_t length = strlen(ssid);
    out[0] = 0;
    out[1] = (uint8_t)length;
    memcpy(out + 2, ssid, length);
    return 2 + length;
}

static size_t append_ds(uint8_t *out, uint8_t channel)
{
    out[0] = 3;
    out[1] = 1;
    out[2] = channel;
    return 3;
}

static size_t append_rsn_wpa2(uint8_t *out)
{
    out[0] = 48;
    out[1] = 20;
    uint8_t *value = out + 2;
    put_le16(value, 1);
    static const uint8_t group[4] = {0x00, 0x0f, 0xac, 0x04};
    memcpy(value + 2, group, 4);
    put_le16(value + 6, 1);
    memcpy(value + 8, group, 4);
    put_le16(value + 12, 1);
    static const uint8_t psk[4] = {0x00, 0x0f, 0xac, 0x02};
    memcpy(value + 14, psk, 4);
    put_le16(value + 18, 0);
    return 22;
}

static size_t build_rsn_akms(uint8_t *out, const uint8_t *akms, size_t count)
{
    static const uint8_t group[4] = {0x00, 0x0f, 0xac, 0x04};
    out[0] = 48;
    out[1] = (uint8_t)(16 + count * 4);
    uint8_t *value = out + 2;
    put_le16(value, 1);
    memcpy(value + 2, group, 4);
    put_le16(value + 6, 1);
    memcpy(value + 8, group, 4);
    put_le16(value + 12, (uint16_t)count);
    for (size_t i = 0; i < count; ++i) {
        value[14 + i * 4] = 0x00;
        value[15 + i * 4] = 0x0f;
        value[16 + i * 4] = 0xac;
        value[17 + i * 4] = akms[i];
    }
    put_le16(value + 14 + count * 4, 0);
    return 18 + count * 4;
}

static void test_rsn_security(void)
{
    uint8_t ie[64];
    uint8_t akms[2];
    nero_wifi_ies parsed;

    akms[0] = 2;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 1), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA2);

    akms[0] = 8;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 1), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA3);

    akms[0] = 2;
    akms[1] = 8;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 2), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA2_WPA3);

    akms[0] = 1;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 1), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA2);

    akms[0] = 12;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 1), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA3);

    akms[0] = 1;
    akms[1] = 12;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 2), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_WPA2_WPA3);

    akms[0] = 18;
    nero_wifi_parse_ies(&parsed, ie, build_rsn_akms(ie, akms, 1), 1);
    CHECK(parsed.security == NERO_WIFI_SEC_OWE);
}

static size_t build_beacon(uint8_t *out)
{
    memset(out, 0, 128);
    put_le16(out, 0x0080); /* management, beacon */
    memcpy(out + 4, BROADCAST_MAC, NERO_WIFI_MAC_LEN);
    memcpy(out + 10, BSSID, NERO_WIFI_MAC_LEN);
    memcpy(out + 16, BSSID, NERO_WIFI_MAC_LEN);
    put_le16(out + 22, 0x0010); /* sequence number 1 */

    uint8_t *body = out + 24;
    memset(body, 0, 8); /* timestamp */
    put_le16(body + 8, 100);
    put_le16(body + 10, 0x0011); /* ESS + privacy */

    uint8_t *ies = body + 12;
    ies += append_ssid(ies, "TestNet");
    ies += append_ds(ies, 6);
    ies += append_rsn_wpa2(ies);
    return (size_t)(ies - out);
}

static size_t build_probe_request(uint8_t *out)
{
    memset(out, 0, 96);
    put_le16(out, 0x0040); /* management, probe request */
    memcpy(out + 4, BROADCAST_MAC, NERO_WIFI_MAC_LEN);
    memcpy(out + 10, CLIENT_MAC, NERO_WIFI_MAC_LEN);
    memcpy(out + 16, BROADCAST_MAC, NERO_WIFI_MAC_LEN);
    put_le16(out + 22, 0x0020);
    uint8_t *ies = out + 24;
    ies += append_ssid(ies, "TestNet");
    return (size_t)(ies - out);
}

static size_t build_assoc_request(uint8_t *out)
{
    memset(out, 0, 96);
    put_le16(out, 0x0000); /* management, association request */
    memcpy(out + 4, BSSID, NERO_WIFI_MAC_LEN);
    memcpy(out + 10, CLIENT_MAC, NERO_WIFI_MAC_LEN);
    memcpy(out + 16, BSSID, NERO_WIFI_MAC_LEN);
    put_le16(out + 22, 0x0030);

    uint8_t *body = out + 24;
    put_le16(body, 0x0011); /* ESS + privacy */
    put_le16(body + 2, 10); /* listen interval */
    uint8_t *ies = body + 4;
    ies += append_ssid(ies, "TestNet");
    ies += append_ds(ies, 6);
    ies += append_rsn_wpa2(ies);
    return (size_t)(ies - out);
}

static size_t build_data(uint8_t *out, int to_ds, int from_ds)
{
    memset(out, 0, 64);
    put_le16(out, (uint16_t)(0x0008 | (to_ds ? 0x0100 : 0) | (from_ds ? 0x0200 : 0)));
    if (to_ds && !from_ds) {
        memcpy(out + 4, BSSID, NERO_WIFI_MAC_LEN);
        memcpy(out + 10, CLIENT_MAC, NERO_WIFI_MAC_LEN);
        memcpy(out + 16, DEST_MAC, NERO_WIFI_MAC_LEN);
    } else {
        memcpy(out + 4, CLIENT_MAC, NERO_WIFI_MAC_LEN);
        memcpy(out + 10, BSSID, NERO_WIFI_MAC_LEN);
        memcpy(out + 16, DEST_MAC, NERO_WIFI_MAC_LEN);
    }
    put_le16(out + 22, 0x0040);
    memset(out + 24, 0xaa, 8); /* opaque protected payload; never decoded */
    return 32;
}

static size_t build_deauth(uint8_t *out)
{
    memset(out, 0, 32);
    put_le16(out, 0x00c0); /* management, deauthentication */
    memcpy(out + 4, CLIENT_MAC, NERO_WIFI_MAC_LEN);
    memcpy(out + 10, BSSID, NERO_WIFI_MAC_LEN);
    memcpy(out + 16, BSSID, NERO_WIFI_MAC_LEN);
    put_le16(out + 22, 0x0050);
    put_le16(out + 24, 3);
    return 26;
}

static size_t wrap_radiotap(uint8_t *out, int frequency, int rssi, const uint8_t *frame,
                            size_t frame_len)
{
    size_t header = build_radiotap(out, frequency, rssi);
    memcpy(out + header, frame, frame_len);
    return header + frame_len;
}

static void test_tracker(void)
{
    uint8_t packet[256];
    uint8_t frame[160];
    uint8_t bssid[NERO_WIFI_MAC_LEN];
    CHECK(nero_wifi_parse_mac("02:11:22:33:44:55", bssid) == 0);
    CHECK(memcmp(bssid, BSSID, NERO_WIFI_MAC_LEN) == 0);
    CHECK(nero_wifi_parse_mac("02-11-22-33-44-55", bssid) == 0);
    CHECK(nero_wifi_parse_mac("021122334455", bssid) == 0);
    CHECK(nero_wifi_parse_mac("not-a-mac", bssid) != 0);

    nero_wifi_tracker *tracker = nero_wifi_tracker_create();
    CHECK(tracker != NULL);
    if (tracker == NULL)
        return;

    size_t frame_len = build_beacon(frame);
    size_t packet_len = wrap_radiotap(packet, 2437, -42, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1000000, NULL, 0) == 1);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1100000, NULL, 0) == 0);

    nero_wifi_ap ap;
    CHECK(nero_wifi_tracker_find_ap(tracker, BSSID, &ap) == 0);
    CHECK(strcmp(ap.ssid, "TestNet") == 0);
    CHECK(ap.has_bssid == 1);
    CHECK(ap.hidden == 0);
    CHECK(ap.has_channel == 1 && ap.channel == 6);
    CHECK(ap.has_frequency == 1 && ap.frequency_mhz == 2437);
    CHECK(ap.band == NERO_WIFI_BAND_2GHZ);
    CHECK(ap.has_rssi == 1 && ap.rssi == -42);
    CHECK(ap.security == NERO_WIFI_SEC_WPA2);
    CHECK(ap.beacon_frames == 1);
    CHECK(ap.rssi_history_count == 1);
    CHECK(ap.client_count == 0);

    size_t extended_header = build_radiotap_extended(packet, 2437, -42);
    memcpy(packet + extended_header, frame, frame_len);
    nero_wifi_radiotap radio;
    CHECK(nero_wifi_parse_radiotap(packet, extended_header + frame_len, &radio, NULL, 0) == 0);
    CHECK(radio.header_len == 28);
    CHECK(radio.has_frequency == 1 && radio.frequency_mhz == 2437);
    CHECK(radio.has_rssi == 1 && radio.rssi == -42);

    frame_len = build_probe_request(frame);
    packet_len = wrap_radiotap(packet, 2437, -61, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1200000, NULL, 0) == 1);

    nero_wifi_client client;
    CHECK(nero_wifi_tracker_find_client(tracker, CLIENT_MAC, &client) == 0);
    CHECK(client.randomized == 1);
    CHECK(client.confidence == NERO_WIFI_CONFIDENCE_OBSERVED);
    CHECK(client.has_bssid == 0);
    CHECK(client.frames == 1);
    CHECK(client.has_channel == 1 && client.channel == 6);
    CHECK(client.band == NERO_WIFI_BAND_2GHZ);

    frame_len = build_assoc_request(frame);
    packet_len = wrap_radiotap(packet, 2437, -48, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1300000, NULL, 0) == 1);
    CHECK(nero_wifi_tracker_find_client(tracker, CLIENT_MAC, &client) == 0);
    CHECK(client.confidence == NERO_WIFI_CONFIDENCE_ASSOCIATED);
    CHECK(client.state == NERO_WIFI_STATE_ASSOCIATED);
    CHECK(client.has_bssid == 1);
    CHECK(memcmp(client.bssid, BSSID, NERO_WIFI_MAC_LEN) == 0);
    CHECK(client.frames == 2);

    CHECK(nero_wifi_tracker_find_ap(tracker, BSSID, &ap) == 0);
    CHECK(ap.client_count == 1);
    CHECK(ap.observed_client_count == 1);

    frame_len = build_data(frame, 0, 1);
    packet_len = wrap_radiotap(packet, 2437, -40, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1400000, NULL, 0) == 1);
    frame_len = build_data(frame, 1, 0);
    packet_len = wrap_radiotap(packet, 2437, -50, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1500000, NULL, 0) == 1);

    CHECK(nero_wifi_tracker_find_client(tracker, CLIENT_MAC, &client) == 0);
    CHECK(client.frames == 4);
    CHECK(client.data_frames == 2);
    CHECK(client.rssi_history_count == 2);
    CHECK(client.rssi == -50);

    frame_len = build_deauth(frame);
    packet_len = wrap_radiotap(packet, 2437, -44, frame, frame_len);
    CHECK(nero_wifi_tracker_ingest_l2(tracker, packet, packet_len, NERO_WIFI_LINKTYPE_RADIOTAP,
                                      1600000, NULL, 0) == 1);
    CHECK(nero_wifi_tracker_find_client(tracker, CLIENT_MAC, &client) == 0);
    CHECK(client.state == NERO_WIFI_STATE_DISASSOCIATED);

    nero_wifi_stats stats;
    nero_wifi_tracker_get_stats(tracker, &stats);
    CHECK(stats.frames == 6);
    CHECK(stats.management == 4);
    CHECK(stats.data == 2);
    CHECK(stats.duplicates == 1);
    CHECK(stats.malformed == 0);

    const uint8_t tiny[5] = {0};
    CHECK(nero_wifi_tracker_ingest_l2(tracker, tiny, sizeof(tiny), NERO_WIFI_LINKTYPE_IEEE802_11,
                                      1700000, NULL, 0) == -1);
    nero_wifi_tracker_get_stats(tracker, &stats);
    CHECK(stats.malformed == 1);

    nero_wifi_tracker_destroy(tracker);
}

static int write_bytes(FILE *file, const void *data, size_t length)
{
    return fwrite(data, 1, length, file) == length ? 0 : -1;
}

#ifndef _WIN32
static void put_le64(uint8_t *out, uint64_t value)
{
    put_le32(out, (uint32_t)(value & 0xffffffffu));
    put_le32(out + 4, (uint32_t)(value >> 32));
}

static void put_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xffu);
}

static void put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)((value >> 16) & 0xffu);
    out[2] = (uint8_t)((value >> 8) & 0xffu);
    out[3] = (uint8_t)(value & 0xffu);
}

static void put_be64(uint8_t *out, uint64_t value)
{
    put_be32(out, (uint32_t)(value >> 32));
    put_be32(out + 4, (uint32_t)(value & 0xffffffffu));
}

static size_t build_classic_pcap(uint8_t *out, size_t capacity)
{
    uint8_t frame[160];
    uint8_t packet[256];
    size_t frame_len = build_beacon(frame);
    size_t packet_len = wrap_radiotap(packet, 2437, -42, frame, frame_len);
    if (capacity < 24 + 16 + packet_len)
        return 0;

    uint8_t *cursor = out;
    put_le32(cursor, 0xa1b2c3d4u); /* little-endian magic bytes d4 c3 b2 a1 */
    put_le16(cursor + 4, 2);
    put_le16(cursor + 6, 4);
    put_le32(cursor + 8, 0);
    put_le32(cursor + 12, 0);
    put_le32(cursor + 16, 65535);
    put_le32(cursor + 20, 0x00010000u | NERO_WIFI_LINKTYPE_RADIOTAP);
    cursor += 24;
    put_le32(cursor, 1700000000u);
    put_le32(cursor + 4, 0);
    put_le32(cursor + 8, (uint32_t)packet_len);
    put_le32(cursor + 12, (uint32_t)packet_len);
    cursor += 16;
    memcpy(cursor, packet, packet_len);
    cursor += packet_len;
    return (size_t)(cursor - out);
}

static size_t build_pcapng(uint8_t *out, size_t capacity)
{
    uint8_t frame[160];
    uint8_t packet[256];
    size_t frame_len = build_beacon(frame);
    size_t packet_len = wrap_radiotap(packet, 2437, -42, frame, frame_len);
    size_t padded = (packet_len + 3) & ~(size_t)3;
    size_t total = 28 + 20 + 32 + padded;
    if (capacity < total)
        return 0;

    uint8_t *cursor = out;
    put_le32(cursor, 0x0a0d0d0au);
    put_le32(cursor + 4, 28);
    put_le32(cursor + 8, 0x1a2b3c4du);
    put_le16(cursor + 12, 1);
    put_le16(cursor + 14, 0);
    put_le64(cursor + 16, UINT64_MAX);
    put_le32(cursor + 24, 28);
    cursor += 28;

    put_le32(cursor, 1); /* Interface Description Block */
    put_le32(cursor + 4, 20);
    put_le16(cursor + 8, NERO_WIFI_LINKTYPE_RADIOTAP);
    put_le16(cursor + 10, 0);
    put_le32(cursor + 12, 65535);
    put_le32(cursor + 16, 20);
    cursor += 20;

    put_le32(cursor, 6); /* Enhanced Packet Block */
    put_le32(cursor + 4, (uint32_t)(32 + padded));
    put_le32(cursor + 8, 0);
    put_le32(cursor + 12, 0);
    put_le32(cursor + 16, 1000000);
    put_le32(cursor + 20, (uint32_t)packet_len);
    put_le32(cursor + 24, (uint32_t)packet_len);
    memcpy(cursor + 28, packet, packet_len);
    memset(cursor + 28 + packet_len, 0, padded - packet_len);
    put_le32(cursor + 28 + padded, (uint32_t)(32 + padded));
    cursor += 32 + padded;
    return (size_t)(cursor - out);
}

static size_t build_pcapng_big_endian(uint8_t *out, size_t capacity)
{
    uint8_t frame[160];
    uint8_t packet[256];
    size_t frame_len = build_beacon(frame);
    size_t packet_len = wrap_radiotap(packet, 2437, -42, frame, frame_len);
    size_t padded = (packet_len + 3) & ~(size_t)3;
    size_t total = 28 + 20 + 32 + padded;
    if (capacity < total)
        return 0;

    uint8_t *cursor = out;
    put_be32(cursor, 0x0a0d0d0au);
    put_be32(cursor + 4, 28);
    put_be32(cursor + 8, 0x1a2b3c4du);
    put_be16(cursor + 12, 1);
    put_be16(cursor + 14, 0);
    put_be64(cursor + 16, UINT64_MAX);
    put_be32(cursor + 24, 28);
    cursor += 28;

    put_be32(cursor, 1); /* Interface Description Block */
    put_be32(cursor + 4, 20);
    put_be16(cursor + 8, NERO_WIFI_LINKTYPE_RADIOTAP);
    put_be16(cursor + 10, 0);
    put_be32(cursor + 12, 65535);
    put_be32(cursor + 16, 20);
    cursor += 20;

    put_be32(cursor, 6); /* Enhanced Packet Block */
    put_be32(cursor + 4, (uint32_t)(32 + padded));
    put_be32(cursor + 8, 0);
    uint64_t timestamp = 1700000000000000ULL;
    put_be32(cursor + 12, (uint32_t)(timestamp >> 32));
    put_be32(cursor + 16, (uint32_t)timestamp);
    put_be32(cursor + 20, (uint32_t)packet_len);
    put_be32(cursor + 24, (uint32_t)packet_len);
    memcpy(cursor + 28, packet, packet_len);
    memset(cursor + 28 + packet_len, 0, padded - packet_len);
    put_be32(cursor + 28 + padded, (uint32_t)(32 + padded));
    cursor += 32 + padded;
    return (size_t)(cursor - out);
}

static void test_pcap_files(void)
{
    uint8_t file_data[2048];
    char path[128];
    snprintf(path, sizeof(path), "/tmp/nero-wifi-test-%ld.pcap", (long)getpid());

    size_t length = build_classic_pcap(file_data, sizeof(file_data));
    CHECK(length > 0);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(write_bytes(file, file_data, length) == 0);
        CHECK(fclose(file) == 0);
        nero_wifi_tracker *tracker = nero_wifi_tracker_create();
        nero_wifi_stats stats;
        char error[256] = {0};
        CHECK(nero_wifi_analyze_pcap(tracker, path, &stats, error, sizeof(error)) == 0);
        CHECK(stats.packets == 1);
        CHECK(stats.frames == 1);
        CHECK(stats.linktype == NERO_WIFI_LINKTYPE_RADIOTAP);
        CHECK(stats.source_is_file == 1);
        nero_wifi_ap ap;
        CHECK(nero_wifi_tracker_find_ap(tracker, BSSID, &ap) == 0);
        CHECK(strcmp(ap.ssid, "TestNet") == 0);
        CHECK(ap.first_seen_us == 1700000000000000LL);
        nero_wifi_tracker_destroy(tracker);
        remove(path);
    }

    snprintf(path, sizeof(path), "/tmp/nero-wifi-test-%ld.pcapng", (long)getpid());
    length = build_pcapng(file_data, sizeof(file_data));
    CHECK(length > 0);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(write_bytes(file, file_data, length) == 0);
        CHECK(fclose(file) == 0);
        nero_wifi_tracker *tracker = nero_wifi_tracker_create();
        nero_wifi_stats stats;
        char error[256] = {0};
        CHECK(nero_wifi_analyze_pcap(tracker, path, &stats, error, sizeof(error)) == 0);
        CHECK(stats.packets == 1);
        CHECK(stats.frames == 1);
        CHECK(stats.linktype == NERO_WIFI_LINKTYPE_RADIOTAP);
        nero_wifi_ap ap;
        CHECK(nero_wifi_tracker_find_ap(tracker, BSSID, &ap) == 0);
        CHECK(ap.beacon_frames == 1);
        nero_wifi_tracker_destroy(tracker);
        remove(path);
    }

    snprintf(path, sizeof(path), "/tmp/nero-wifi-test-%ld-be.pcapng", (long)getpid());
    length = build_pcapng_big_endian(file_data, sizeof(file_data));
    CHECK(length > 0);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(write_bytes(file, file_data, length) == 0);
        CHECK(fclose(file) == 0);
        nero_wifi_tracker *tracker = nero_wifi_tracker_create();
        nero_wifi_stats stats;
        char error[256] = {0};
        CHECK(nero_wifi_analyze_pcap(tracker, path, &stats, error, sizeof(error)) == 0);
        CHECK(stats.packets == 1);
        CHECK(stats.frames == 1);
        CHECK(stats.linktype == NERO_WIFI_LINKTYPE_RADIOTAP);
        nero_wifi_ap ap;
        CHECK(nero_wifi_tracker_find_ap(tracker, BSSID, &ap) == 0);
        CHECK(ap.beacon_frames == 1);
        CHECK(ap.first_seen_us == 1700000000000000LL);
        nero_wifi_tracker_destroy(tracker);
        remove(path);
    }
}
#endif

int main(void)
{
    test_rsn_security();
    test_tracker();
#ifndef _WIN32
    test_pcap_files();
#endif
    if (failures != 0) {
        fprintf(stderr, "%d Wi-Fi test failure(s)\n", failures);
        return 1;
    }
    puts("wifi-core: all checks passed");
    return 0;
}
