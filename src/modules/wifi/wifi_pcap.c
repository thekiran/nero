/* Offline classic pcap and pcapng analysis.
 *
 * The reader streams records, enforces bounds on every length field and never
 * allocates more than a modest per-block working buffer. Ethernet captures are
 * rejected because they do not contain IEEE 802.11 frame headers. */

#include "nero/wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PCAP_MAX_RECORD (16u * 1024u * 1024u)
#define PCAPNG_MAX_BLOCK (32u * 1024u * 1024u)
#define PCAPNG_MAX_INTERFACES 32
#define PCAPNG_SHB_MAGIC "\x0a\x0d\x0d\x0a"

typedef struct {
    uint16_t linktype;
    uint32_t snaplen;
    uint8_t tsresol;
} pcapng_interface;

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static uint16_t read16(const uint8_t *data, int little)
{
    if (little)
        return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read32(const uint8_t *data, int little)
{
    if (little) {
        return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
               ((uint32_t)data[3] << 24);
    }
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

/* 1 = complete, 0 = clean EOF before any byte, -1 = short/error. */
static int read_exact(FILE *file, void *buffer, size_t length)
{
    size_t read = fread(buffer, 1, length, file);
    if (read == length)
        return 1;
    if (read == 0 && feof(file))
        return 0;
    return -1;
}

static int linktype_supported(uint32_t linktype)
{
    return linktype == NERO_WIFI_LINKTYPE_IEEE802_11 || linktype == NERO_WIFI_LINKTYPE_RADIOTAP;
}

static void merge_tracker_stats(nero_wifi_tracker *tracker, nero_wifi_stats *stats)
{
    nero_wifi_stats current;
    nero_wifi_tracker_get_stats(tracker, &current);
    stats->frames = current.frames;
    stats->management = current.management;
    stats->control = current.control;
    stats->data = current.data;
    stats->duplicates = current.duplicates;
    stats->dropped += current.dropped;
    stats->evicted = current.evicted;
    stats->malformed = current.malformed;
}

static void process_packet(nero_wifi_tracker *tracker, const uint8_t *data, size_t caplen,
                           uint32_t linktype, int64_t timestamp_us, nero_wifi_stats *stats)
{
    ++stats->packets;
    stats->bytes += caplen;
    char error[128] = {0};
    if (!linktype_supported(linktype)) {
        ++stats->non_wifi;
        return;
    }
    if (stats->linktype == 0)
        stats->linktype = (int)linktype;
    if (nero_wifi_tracker_ingest_l2(tracker, data, caplen, (int)linktype, timestamp_us, error,
                                    sizeof(error)) != 1 &&
        error[0] != '\0') {
        /* Malformed-frame accounting is performed by the tracker. */
    }
}

static int64_t classic_timestamp(uint32_t seconds, uint32_t fraction, int nanosecond)
{
    /* uint32 seconds cover 1970-2106; multiplying by 1e6 stays well inside
     * int64 range, so modern capture timestamps must not be discarded. */
    int64_t base = (int64_t)seconds * 1000000LL;
    if (nanosecond)
        return base + fraction / 1000;
    return base + fraction;
}

static int parse_classic_pcap(FILE *file, const uint8_t *header, nero_wifi_tracker *tracker,
                              nero_wifi_stats *stats, char *error, size_t error_len)
{
    uint32_t magic = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                     ((uint32_t)header[2] << 8) | (uint32_t)header[3];
    int little;
    int nanosecond = 0;
    if (magic == 0xa1b2c3d4u) {
        little = 0;
    } else if (magic == 0xd4c3b2a1u) {
        little = 1;
    } else if (magic == 0xa1b23c4du) {
        little = 0;
        nanosecond = 1;
    } else if (magic == 0x4d3cb2a1u) {
        little = 1;
        nanosecond = 1;
    } else {
        set_error(error, error_len, "unrecognized pcap magic");
        return -1;
    }

    uint32_t snaplen = read32(header + 16, little);
    uint32_t linktype = read32(header + 20, little) & 0x0000ffffu;
    if (snaplen == 0 || snaplen > PCAP_MAX_RECORD) {
        set_error(error, error_len, "invalid pcap snapshot length");
        return -1;
    }
    if (!linktype_supported(linktype)) {
        char message[160];
        snprintf(message, sizeof(message),
                 "unsupported pcap link type %u (need 105 IEEE802_11 or 127 radiotap; "
                 "an Ethernet capture has no 802.11 headers)",
                 linktype);
        set_error(error, error_len, message);
        return -1;
    }
    stats->linktype = (int)linktype;

    uint8_t *buffer = malloc(snaplen);
    if (buffer == NULL) {
        set_error(error, error_len, "out of memory");
        return -1;
    }

    int status = 0;
    for (;;) {
        uint8_t record[16];
        int got = read_exact(file, record, sizeof(record));
        if (got == 0)
            break;
        if (got < 0) {
            set_error(error, error_len, "truncated pcap packet header");
            status = -1;
            break;
        }
        uint32_t seconds = read32(record, little);
        uint32_t fraction = read32(record + 4, little);
        uint32_t captured = read32(record + 8, little);
        uint32_t original = read32(record + 12, little);
        if (captured > PCAP_MAX_RECORD || captured > snaplen || captured > original) {
            set_error(error, error_len, "invalid pcap packet length");
            status = -1;
            break;
        }
        if (captured > 0) {
            got = read_exact(file, buffer, captured);
            if (got != 1) {
                set_error(error, error_len, "truncated pcap packet data");
                status = -1;
                break;
            }
        }
        process_packet(tracker, buffer, captured, linktype,
                       classic_timestamp(seconds, fraction, nanosecond), stats);
    }

    free(buffer);
    return status;
}

static int64_t pcapng_timestamp(uint32_t high, uint32_t low, uint8_t resolution)
{
    uint64_t value = ((uint64_t)high << 32) | low;
    if ((resolution & 0x80u) != 0) {
        /* Binary resolution (2^-n) is rare; approximate in microseconds. */
        unsigned int power = resolution & 0x7fu;
        if (power >= 63 || value > (uint64_t)INT64_MAX / 1000000ULL)
            return INT64_MAX;
        uint64_t scale = (uint64_t)1 << power;
        return (int64_t)((value * 1000000ULL) / scale);
    }
    if (resolution <= 6) {
        uint64_t multiplier = 1;
        for (uint8_t i = resolution; i < 6; ++i)
            multiplier *= 10;
        if (value > (uint64_t)INT64_MAX / multiplier)
            return INT64_MAX;
        return (int64_t)(value * multiplier);
    }
    uint64_t divisor = 1;
    for (uint8_t i = 6; i < resolution && i < 19; ++i)
        divisor *= 10;
    return (int64_t)(value / divisor);
}

static int pcapng_byte_order(const uint8_t bom[4], int *little)
{
    if (bom[0] == 0x4d && bom[1] == 0x3c && bom[2] == 0x2b && bom[3] == 0x1a) {
        *little = 1;
        return 0;
    }
    if (bom[0] == 0x1a && bom[1] == 0x2b && bom[2] == 0x3c && bom[3] == 0x4d) {
        *little = 0;
        return 0;
    }
    return -1;
}

static void parse_idb(const uint8_t *body, size_t body_len, pcapng_interface *interfaces,
                      int *interface_count, int little)
{
    if (body_len < 8 || *interface_count >= PCAPNG_MAX_INTERFACES)
        return;
    pcapng_interface *entry = &interfaces[*interface_count];
    entry->linktype = read16(body, little);
    entry->snaplen = read32(body + 4, little);
    entry->tsresol = 6;
    size_t offset = 8;
    while (offset + 4 <= body_len) {
        uint16_t code = read16(body + offset, little);
        uint16_t length = read16(body + offset + 2, little);
        offset += 4;
        if ((size_t)length > body_len - offset)
            break;
        if (code == 9 && length >= 1)
            entry->tsresol = body[offset];
        offset += ((size_t)length + 3) & ~(size_t)3;
    }
    ++*interface_count;
}

static int parse_pcapng(FILE *file, nero_wifi_tracker *tracker, nero_wifi_stats *stats, char *error,
                        size_t error_len)
{
    uint8_t header[8];
    uint8_t bom[4];
    if (read_exact(file, header, sizeof(header)) != 1 || read_exact(file, bom, sizeof(bom)) != 1) {
        set_error(error, error_len, "truncated pcapng section header");
        return -1;
    }
    if (memcmp(header, PCAPNG_SHB_MAGIC, 4) != 0) {
        set_error(error, error_len, "invalid pcapng section header");
        return -1;
    }
    int little;
    if (pcapng_byte_order(bom, &little) != 0) {
        set_error(error, error_len, "invalid pcapng byte-order magic");
        return -1;
    }
    uint32_t total = read32(header + 4, little);
    if (total < 28 || total > PCAPNG_MAX_BLOCK || (total % 4) != 0) {
        set_error(error, error_len, "invalid pcapng section length");
        return -1;
    }
    size_t remaining = total - 12;
    uint8_t *section = malloc(remaining);
    if (section == NULL) {
        set_error(error, error_len, "out of memory");
        return -1;
    }
    if (read_exact(file, section, remaining) != 1) {
        free(section);
        set_error(error, error_len, "truncated pcapng section header");
        return -1;
    }
    free(section);

    pcapng_interface interfaces[PCAPNG_MAX_INTERFACES];
    int interface_count = 0;
    int status = 0;
    int saw_supported = 0;

    for (;;) {
        int got = read_exact(file, header, sizeof(header));
        if (got == 0)
            break;
        if (got < 0) {
            set_error(error, error_len, "truncated pcapng block header");
            status = -1;
            break;
        }

        int is_section = memcmp(header, PCAPNG_SHB_MAGIC, 4) == 0;
        if (is_section) {
            if (read_exact(file, bom, sizeof(bom)) != 1) {
                set_error(error, error_len, "truncated pcapng section header");
                status = -1;
                break;
            }
            if (pcapng_byte_order(bom, &little) != 0) {
                set_error(error, error_len, "invalid pcapng byte-order magic");
                status = -1;
                break;
            }
            total = read32(header + 4, little);
            if (total < 28 || total > PCAPNG_MAX_BLOCK || (total % 4) != 0) {
                set_error(error, error_len, "invalid pcapng section length");
                status = -1;
                break;
            }
            remaining = total - 12;
            uint8_t *rest = malloc(remaining);
            if (rest == NULL) {
                set_error(error, error_len, "out of memory");
                status = -1;
                break;
            }
            if (read_exact(file, rest, remaining) != 1) {
                free(rest);
                set_error(error, error_len, "truncated pcapng section");
                status = -1;
                break;
            }
            free(rest);
            interface_count = 0;
            continue;
        }

        total = read32(header + 4, little);
        if (total < 12 || total > PCAPNG_MAX_BLOCK || (total % 4) != 0) {
            set_error(error, error_len, "invalid pcapng block length");
            status = -1;
            break;
        }
        size_t rest_len = total - 8;
        uint8_t *block = malloc(rest_len);
        if (block == NULL) {
            set_error(error, error_len, "out of memory");
            status = -1;
            break;
        }
        if (read_exact(file, block, rest_len) != 1) {
            free(block);
            set_error(error, error_len, "truncated pcapng block");
            status = -1;
            break;
        }
        size_t body_len = total - 12;
        uint32_t trailer = read32(block + body_len, little);
        if (trailer != total) {
            free(block);
            set_error(error, error_len, "pcapng block length mismatch");
            status = -1;
            break;
        }

        uint32_t type = read32(header, little);
        if (type == 1) {
            parse_idb(block, body_len, interfaces, &interface_count, little);
        } else if (type == 6 && body_len >= 20) {
            uint32_t interface_id = read32(block, little);
            uint32_t ts_high = read32(block + 4, little);
            uint32_t ts_low = read32(block + 8, little);
            uint32_t caplen = read32(block + 12, little);
            if (interface_id < (uint32_t)interface_count) {
                const pcapng_interface *entry = &interfaces[interface_id];
                if (linktype_supported(entry->linktype) && caplen <= body_len - 20 &&
                    caplen <= PCAP_MAX_RECORD) {
                    saw_supported = 1;
                    process_packet(tracker, block + 20, caplen, entry->linktype,
                                   pcapng_timestamp(ts_high, ts_low, entry->tsresol), stats);
                } else {
                    ++stats->non_wifi;
                }
            } else {
                ++stats->non_wifi;
            }
        } else if (type == 3 && body_len >= 4) {
            uint32_t original = read32(block, little);
            size_t caplen = body_len - 4;
            if (caplen > original)
                caplen = original;
            if (interface_count > 0 && linktype_supported(interfaces[0].linktype) &&
                caplen <= PCAP_MAX_RECORD) {
                saw_supported = 1;
                process_packet(tracker, block + 4, caplen, interfaces[0].linktype,
                               nero_wifi_now_us(), stats);
            } else {
                ++stats->non_wifi;
            }
        } else if (type == 2 && body_len >= 20) {
            uint16_t interface_id = read16(block, little);
            uint32_t ts_high = read32(block + 4, little);
            uint32_t ts_low = read32(block + 8, little);
            uint32_t caplen = read32(block + 12, little);
            if (interface_id < interface_count) {
                const pcapng_interface *entry = &interfaces[interface_id];
                if (linktype_supported(entry->linktype) && caplen <= body_len - 20 &&
                    caplen <= PCAP_MAX_RECORD) {
                    saw_supported = 1;
                    process_packet(tracker, block + 20, caplen, entry->linktype,
                                   pcapng_timestamp(ts_high, ts_low, entry->tsresol), stats);
                } else {
                    ++stats->non_wifi;
                }
            } else {
                ++stats->non_wifi;
            }
        }
        free(block);
    }

    if (status == 0 && stats->packets > 0 && !saw_supported) {
        set_error(error, error_len,
                  "pcapng contains no IEEE 802.11 interface (need linktype 105 or 127)");
        status = -1;
    }
    if (interface_count > 0 && stats->linktype == 0)
        stats->linktype = interfaces[0].linktype;
    return status;
}

int nero_wifi_analyze_pcap(nero_wifi_tracker *tracker, const char *path, nero_wifi_stats *stats,
                           char *error, size_t error_len)
{
    if (tracker == NULL || path == NULL || stats == NULL) {
        set_error(error, error_len, "invalid argument");
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->source_is_file = 1;
    stats->started_us = nero_wifi_now_us();
    snprintf(stats->source, sizeof(stats->source), "%s", path);

    /* Open first, then validate the opened handle with fstat: checking the path
     * with stat() before fopen() leaves a time-of-check/time-of-use window where
     * the path could be swapped for a device or a different file. */
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        char message[256];
        snprintf(message, sizeof(message), "cannot open %s: %s", path, strerror(errno));
        set_error(error, error_len, message);
        return -1;
    }
    struct stat info;
    if (fstat(fileno(file), &info) != 0) {
        char message[256];
        snprintf(message, sizeof(message), "cannot stat %s: %s", path, strerror(errno));
        set_error(error, error_len, message);
        fclose(file);
        return -1;
    }
    if (!S_ISREG(info.st_mode)) {
        set_error(error, error_len, "pcap source is not a regular file");
        fclose(file);
        return -1;
    }

    uint8_t header[24];
    int got = read_exact(file, header, sizeof(header));
    if (got != 1) {
        fclose(file);
        set_error(error, error_len, "truncated pcap global header");
        return -1;
    }

    int status;
    if (memcmp(header, PCAPNG_SHB_MAGIC, 4) == 0) {
        if (fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            set_error(error, error_len, "cannot rewind pcapng file");
            return -1;
        }
        status = parse_pcapng(file, tracker, stats, error, error_len);
    } else {
        status = parse_classic_pcap(file, header, tracker, stats, error, error_len);
    }
    fclose(file);

    stats->ended_us = nero_wifi_now_us();
    if (status == 0)
        merge_tracker_stats(tracker, stats);
    return status;
}
