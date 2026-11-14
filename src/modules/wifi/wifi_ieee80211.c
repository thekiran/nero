/* IEEE 802.11 / radiotap decoding.
 *
 * Bounds-checked, allocation-free and tolerant of malformed input. The parser
 * only interprets frame headers and unencrypted management information
 * elements; protected payloads are never decrypted. */

#include "nero/wifi.h"
#include "wifi_internal.h"

#include <stdio.h>
#include <string.h>

#define WIFI_RADIOTAP_MAX_WORDS 8
#define WIFI_IE_SSID 0
#define WIFI_IE_DS_PARAMS 3
#define WIFI_IE_RSN 48
#define WIFI_IE_VENDOR 221
#define WIFI_OUI_MICROSOFT 0x0050f2

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0) {
        snprintf(error, error_len, "%s", message);
    }
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t *data)
{
    return (uint64_t)read_le32(data) | ((uint64_t)read_le32(data + 4) << 32);
}

static int align_offset(size_t *offset, size_t alignment)
{
    size_t remainder = *offset % alignment;
    if (remainder != 0) {
        *offset += alignment - remainder;
    }
    return 0;
}

static int skip_field(size_t *offset, size_t total, size_t alignment, size_t size)
{
    align_offset(offset, alignment);
    if (*offset > total || size > total - *offset) {
        return -1;
    }
    *offset += size;
    return 0;
}

static int radiotap_field_size(unsigned int bit, size_t *alignment, size_t *size)
{
    /* Sizes and alignment from the radiotap specification. Fields that are
     * variable length or not modelled here are rejected rather than guessed. */
    switch (bit) {
    case 0:
        *alignment = 8;
        *size = 8;
        break; /* TSFT */
    case 1:
        *alignment = 1;
        *size = 1;
        break; /* Flags */
    case 2:
        *alignment = 1;
        *size = 1;
        break; /* Rate */
    case 3:
        *alignment = 2;
        *size = 4;
        break; /* Channel */
    case 4:
        *alignment = 2;
        *size = 2;
        break; /* FHSS */
    case 5:
        *alignment = 1;
        *size = 1;
        break; /* dBm antenna signal */
    case 6:
        *alignment = 1;
        *size = 1;
        break; /* dBm antenna noise */
    case 7:
        *alignment = 2;
        *size = 2;
        break; /* lock quality */
    case 8:
        *alignment = 2;
        *size = 2;
        break; /* TX attenuation */
    case 9:
        *alignment = 2;
        *size = 2;
        break; /* dB TX attenuation */
    case 10:
        *alignment = 1;
        *size = 1;
        break; /* dBm TX power */
    case 11:
        *alignment = 1;
        *size = 1;
        break; /* antenna */
    case 12:
        *alignment = 1;
        *size = 1;
        break; /* dB antenna signal */
    case 13:
        *alignment = 1;
        *size = 1;
        break; /* dB antenna noise */
    case 14:
        *alignment = 2;
        *size = 2;
        break; /* RX flags */
    case 15:
        *alignment = 2;
        *size = 2;
        break; /* TX flags */
    case 16:
        *alignment = 1;
        *size = 1;
        break; /* RTS retries */
    case 17:
        *alignment = 1;
        *size = 1;
        break; /* data retries */
    case 18:
        *alignment = 4;
        *size = 8;
        break; /* XChannel */
    case 19:
        *alignment = 1;
        *size = 3;
        break; /* MCS */
    case 20:
        *alignment = 4;
        *size = 8;
        break; /* A-MPDU status */
    case 21:
        *alignment = 2;
        *size = 12;
        break; /* VHT */
    case 22:
        *alignment = 8;
        *size = 12;
        break; /* timestamp */
    case 23:
        *alignment = 2;
        *size = 12;
        break; /* HE */
    case 24:
        *alignment = 2;
        *size = 12;
        break; /* HE-MU */
    case 25:
        *alignment = 2;
        *size = 6;
        break; /* HE-MU other user */
    case 26:
        *alignment = 1;
        *size = 1;
        break; /* zero-length PSDU */
    case 27:
        *alignment = 4;
        *size = 4;
        break; /* L-SIG */
    default:
        return -1;
    }
    return 0;
}

nero_wifi_band nero_wifi_frequency_to_band(int frequency_mhz)
{
    if (frequency_mhz >= 2400 && frequency_mhz <= 2500)
        return NERO_WIFI_BAND_2GHZ;
    if (frequency_mhz >= 4900 && frequency_mhz <= 5895)
        return NERO_WIFI_BAND_5GHZ;
    if (frequency_mhz >= 5925 && frequency_mhz <= 7125)
        return NERO_WIFI_BAND_6GHZ;
    return NERO_WIFI_BAND_UNKNOWN;
}

int nero_wifi_frequency_to_channel(int frequency_mhz, nero_wifi_band *band)
{
    nero_wifi_band detected = nero_wifi_frequency_to_band(frequency_mhz);
    if (band != NULL)
        *band = detected;
    switch (detected) {
    case NERO_WIFI_BAND_2GHZ:
        if (frequency_mhz == 2484)
            return 14;
        return (frequency_mhz - 2407) / 5;
    case NERO_WIFI_BAND_5GHZ:
        return (frequency_mhz - 5000) / 5;
    case NERO_WIFI_BAND_6GHZ:
        return (frequency_mhz - 5950) / 5;
    default:
        return 0;
    }
}

int nero_wifi_channel_to_frequency(int channel, nero_wifi_band band)
{
    switch (band) {
    case NERO_WIFI_BAND_2GHZ:
        if (channel == 14)
            return 2484;
        if (channel >= 1 && channel <= 13)
            return 2407 + channel * 5;
        break;
    case NERO_WIFI_BAND_5GHZ:
        if (channel >= 32 && channel <= 177)
            return 5000 + channel * 5;
        break;
    case NERO_WIFI_BAND_6GHZ:
        if (channel >= 1 && channel <= 233)
            return 5950 + channel * 5;
        break;
    case NERO_WIFI_BAND_UNKNOWN:
        break;
    }
    return 0;
}

int nero_wifi_parse_radiotap(const uint8_t *data, size_t len, nero_wifi_radiotap *out, char *error,
                             size_t error_len)
{
    if (data == NULL || out == NULL) {
        set_error(error, error_len, "null radiotap buffer");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (len < 8) {
        set_error(error, error_len, "truncated radiotap header");
        return -1;
    }
    if (data[0] != 0) {
        set_error(error, error_len, "unsupported radiotap version");
        return -1;
    }

    uint32_t present[WIFI_RADIOTAP_MAX_WORDS] = {0};
    size_t offset = 4;
    size_t words = 0;
    int more = 1;
    while (more) {
        if (words >= WIFI_RADIOTAP_MAX_WORDS || offset + 4 > len) {
            set_error(error, error_len, "invalid radiotap present bitmap");
            return -1;
        }
        present[words] = read_le32(data + offset);
        offset += 4;
        more = (present[words] & 0x80000000u) != 0;
        ++words;
    }

    uint16_t header_length = read_le16(data + 2);
    if (header_length < offset || header_length > len) {
        set_error(error, error_len, "invalid radiotap length");
        return -1;
    }

    uint16_t channel_flags = 0;
    int unsupported_tail = 0;
    for (size_t word = 0; word < words && !unsupported_tail; ++word) {
        for (unsigned int bit = 0; bit < 32; ++bit) {
            if ((present[word] & (1u << bit)) == 0)
                continue;
            unsigned int field = (unsigned int)(word * 32 + bit);
            if (field == 31)
                continue; /* extended bitmap marker, no payload */
            size_t alignment, size;
            if (radiotap_field_size(field, &alignment, &size) != 0) {
                /* Extended fields (S1G/EHT/vendor TLVs) are variable length and
                 * sit after the standard fields we consume. The radiotap header
                 * length still gives us the frame offset, so keep the decoded
                 * base metadata instead of rejecting an otherwise usable frame. */
                unsupported_tail = 1;
                break;
            }
            if (skip_field(&offset, header_length, alignment, size) != 0) {
                set_error(error, error_len, "truncated radiotap field");
                return -1;
            }
            size_t field_offset = offset - size;
            if (field == 0) {
                out->has_tsft = 1;
                out->tsft = read_le64(data + field_offset);
            } else if (field == 1) {
                out->has_flags = 1;
                out->flags = data[field_offset];
                out->fcs_at_end = (data[field_offset] & 0x10u) != 0;
            } else if (field == 3) {
                out->has_frequency = 1;
                out->frequency_mhz = read_le16(data + field_offset);
                channel_flags = read_le16(data + field_offset + 2);
                out->band = nero_wifi_frequency_to_band(out->frequency_mhz);
                out->has_channel = 1;
                out->channel = nero_wifi_frequency_to_channel(out->frequency_mhz, NULL);
            } else if (field == 5) {
                out->has_rssi = 1;
                out->rssi = (int)(int8_t)data[field_offset];
            } else if (field == 6) {
                out->has_noise = 1;
                out->noise = (int)(int8_t)data[field_offset];
            }
        }
    }

    /* Older captures may omit the frequency but retain the channel flags. */
    if (!out->has_frequency && channel_flags != 0) {
        if (channel_flags & 0x0008u)
            out->band = NERO_WIFI_BAND_2GHZ;
        else if (channel_flags & 0x0010u)
            out->band = NERO_WIFI_BAND_5GHZ;
    }

    out->header_len = header_length;
    return 0;
}

static void copy_ssid(char out[NERO_WIFI_SSID_CAP], uint8_t *out_len, const uint8_t *value,
                      size_t length, int *hidden)
{
    if (length > NERO_WIFI_SSID_MAX)
        length = NERO_WIFI_SSID_MAX;
    int all_zero = length > 0;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (length == 0 || all_zero) {
        out[0] = '\0';
        *out_len = 0;
        *hidden = 1;
        return;
    }
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = value[i];
        /* Control characters are replaced for terminal safety. SSIDs are
         * opaque bytes; the display remains printable and unambiguous. */
        out[written++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out[written] = '\0';
    *out_len = (uint8_t)written;
    *hidden = 0;
}

static void parse_rsn(nero_wifi_ies *out, const uint8_t *value, size_t length)
{
    if (length < 2)
        return;
    uint16_t version = read_le16(value);
    if (version != 1)
        return;

    size_t offset = 2;
    if (offset + 4 > length)
        return;
    offset += 4; /* group cipher suite */

    if (offset + 2 > length)
        return;
    uint16_t pairwise_count = read_le16(value + offset);
    offset += 2;
    if (pairwise_count > 64 || offset + (size_t)pairwise_count * 4 > length)
        return;
    offset += (size_t)pairwise_count * 4;

    if (offset + 2 > length)
        return;
    uint16_t akm_count = read_le16(value + offset);
    offset += 2;
    if (akm_count > 64 || offset + (size_t)akm_count * 4 > length)
        return;

    int saw_psk = 0;
    int saw_sae = 0;
    int saw_owe = 0;
    int saw_wpa2_enterprise = 0;
    int saw_wpa3_enterprise = 0;
    for (uint16_t i = 0; i < akm_count; ++i) {
        const uint8_t *suite = value + offset + (size_t)i * 4;
        if (suite[0] != 0x00 || suite[1] != 0x0f || suite[2] != 0xac)
            continue;
        switch (suite[3]) {
        case 1:
        case 3:
            saw_wpa2_enterprise = 1;
            break; /* 802.1X variants */
        case 2:
        case 4:
        case 6:
            saw_psk = 1;
            break; /* PSK variants */
        case 5:
        case 11:
        case 12:
            saw_wpa3_enterprise = 1;
            break; /* 802.1X SHA-256 / Suite-B variants */
        case 8:
        case 9:
            saw_sae = 1;
            break; /* SAE variants */
        case 18:
            saw_owe = 1;
            break; /* OWE */
        default:
            break;
        }
    }

    if (saw_owe && !saw_psk && !saw_sae && !saw_wpa2_enterprise && !saw_wpa3_enterprise) {
        out->security = NERO_WIFI_SEC_OWE;
    } else if ((saw_sae || saw_wpa3_enterprise) && (saw_psk || saw_wpa2_enterprise)) {
        out->security = NERO_WIFI_SEC_WPA2_WPA3;
    } else if (saw_sae || saw_wpa3_enterprise) {
        out->security = NERO_WIFI_SEC_WPA3;
    } else {
        out->security = NERO_WIFI_SEC_WPA2;
    }
}

void nero_wifi_parse_ies(nero_wifi_ies *out, const uint8_t *data, size_t len, int privacy)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->security = NERO_WIFI_SEC_OPEN;

    int saw_wpa = 0;
    int saw_rsn = 0;
    int saw_ssid = 0;

    size_t offset = 0;
    while (offset + 2 <= len) {
        uint8_t id = data[offset++];
        uint8_t length = data[offset++];
        if ((size_t)length > len - offset)
            break;
        const uint8_t *value = data + offset;

        if (id == WIFI_IE_SSID && !saw_ssid) {
            saw_ssid = 1;
            out->has_ssid = 1;
            copy_ssid(out->ssid, &out->ssid_len, value, length, &out->hidden);
        } else if (id == WIFI_IE_DS_PARAMS && length >= 1) {
            out->has_ds_channel = 1;
            out->ds_channel = value[0];
        } else if (id == WIFI_IE_RSN && !saw_rsn) {
            saw_rsn = 1;
            parse_rsn(out, value, length);
        } else if (id == WIFI_IE_VENDOR && length >= 4 && !saw_wpa) {
            uint32_t oui =
                ((uint32_t)value[0] << 16) | ((uint32_t)value[1] << 8) | (uint32_t)value[2];
            if (oui == WIFI_OUI_MICROSOFT && value[3] == 1) {
                saw_wpa = 1;
            }
        }
        offset += length;
    }

    if (saw_rsn) {
        /* parse_rsn already selected WPA2/WPA3/OWE. */
        return;
    }
    if (saw_wpa) {
        out->security = NERO_WIFI_SEC_WPA;
        return;
    }
    if (privacy) {
        out->security = NERO_WIFI_SEC_WEP;
    }
}

int nero_wifi_parse_assoc_request(const uint8_t *data, size_t len, nero_wifi_ies *out)
{
    if (data == NULL || out == NULL || len < 4)
        return -1;
    uint16_t capability = read_le16(data);
    int privacy = (capability & 0x0010u) != 0;
    nero_wifi_parse_ies(out, data + 4, len - 4, privacy);
    if (privacy && out->security == NERO_WIFI_SEC_OPEN) {
        out->security = NERO_WIFI_SEC_WEP;
    }
    return 0;
}

int nero_wifi_parse_frame(const uint8_t *data, size_t len, nero_wifi_frame *out, char *error,
                          size_t error_len)
{
    if (data == NULL || out == NULL) {
        set_error(error, error_len, "null 802.11 buffer");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (len < 10) {
        set_error(error, error_len, "truncated 802.11 header");
        return -1;
    }

    uint16_t fc = read_le16(data);
    out->type = (uint8_t)((fc >> 2) & 0x3);
    out->subtype = (uint8_t)((fc >> 4) & 0xf);
    out->to_ds = (fc & 0x0100u) != 0;
    out->from_ds = (fc & 0x0200u) != 0;
    out->retry = (fc & 0x0800u) != 0;
    out->protected_frame = (fc & 0x4000u) != 0;

    size_t header_len = 10;
    if (out->type == 2) {
        header_len = 24;
        if (out->to_ds && out->from_ds)
            header_len += 6;
        if ((out->subtype & 0x8u) != 0)
            header_len += 2; /* QoS */
        if ((fc & 0x8000u) != 0 && (out->subtype & 0x8u) != 0)
            header_len += 4;
    } else if (out->type == 0) {
        header_len = 24;
    } else if (out->type == 1) {
        header_len = 10;
    } else {
        set_error(error, error_len, "unsupported 802.11 frame type");
        return -1;
    }

    if (len < header_len) {
        set_error(error, error_len, "truncated 802.11 addresses");
        return -1;
    }

    if (out->type != 1) {
        memcpy(out->addr1, data + 4, NERO_WIFI_MAC_LEN);
        memcpy(out->addr2, data + 10, NERO_WIFI_MAC_LEN);
        memcpy(out->addr3, data + 16, NERO_WIFI_MAC_LEN);
        if (header_len >= 30) {
            memcpy(out->addr4, data + 24, NERO_WIFI_MAC_LEN);
            out->has_addr4 = 1;
        }
        out->sequence = (uint16_t)(((uint16_t)data[22] | ((uint16_t)data[23] << 8)) >> 4);
        out->fragment = (uint16_t)(data[23] & 0x0fu);
    } else {
        /* Control frames carry receiver/transmitter where present. */
        memcpy(out->addr1, data + 4, NERO_WIFI_MAC_LEN);
        if (len >= 16)
            memcpy(out->addr2, data + 10, NERO_WIFI_MAC_LEN);
    }

    out->header_len = header_len;
    out->body = data + header_len;
    out->body_len = len - header_len;
    return 0;
}
