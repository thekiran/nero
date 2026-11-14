/* Linux nl80211/Generic Netlink interface control.
 *
 * The wire format is defined locally so the protocol core can be compiled and
 * tested on non-Linux hosts. Only the socket transport is platform-specific;
 * on macOS/Windows the transport reports that nl80211 is unavailable and no
 * interface state is ever changed. */

#include "nero/wifi_nl80211.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NERO_SAFE_TEST) || defined(NERO_NETWORK_UNAVAILABLE)
#define NERO_NL80211_OFFLINE 1
#endif

#if defined(_WIN32)
/* WIN32_LEAN_AND_MEAN keeps <ole2.h> (and its `interface` macro, which would
 * clash with the `const char *interface` parameters below) out of <windows.h>. */
#include "windows/nero_win_compat.h"
#elif !defined(NERO_NL80211_OFFLINE)
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if !defined(NERO_NL80211_OFFLINE)
/* Netlink/Generic Netlink UAPI values. They are stable ABI. */
#define NERO_AF_NETLINK 16
#define NERO_NETLINK_GENERIC 16
#define NERO_GENL_ID_CTRL 16
#define NERO_GENL_CMD_GETFAMILY 3
#define NERO_CTRL_ATTR_FAMILY_ID 1
#define NERO_CTRL_ATTR_FAMILY_NAME 2

#define NERO_NLM_F_REQUEST 0x01
#define NERO_NLM_F_ACK 0x04
#define NERO_NLMSG_NOOP 1
#define NERO_NLMSG_ERROR 2
#define NERO_NLMSG_DONE 3

#define NERO_NLMSG_ALIGNTO 4
#define NERO_NLMSG_HDRLEN 16
#define NERO_GENL_HDRLEN 4
#define NERO_NLA_HDRLEN 4
#define NERO_NLA_TYPE_MASK 0x3fffu
#define NERO_NLMSG_ALIGN(len) (((len) + NERO_NLMSG_ALIGNTO - 1) & ~(NERO_NLMSG_ALIGNTO - 1))
#define NERO_NLA_ALIGN(len) NERO_NLMSG_ALIGN(len)

#define NERO_NL80211_CMD_GET_WIPHY 1
#define NERO_NL80211_CMD_GET_INTERFACE 5
#define NERO_NL80211_CMD_SET_INTERFACE 6
#define NERO_NL80211_CMD_SET_CHANNEL 65

#define NERO_NL80211_ATTR_WIPHY_FREQ 38
#define NERO_NL80211_ATTR_WIPHY_CHANNEL_TYPE 39
#define NERO_NL80211_CHAN_NO_HT 0

#define NERO_NL80211_ATTR_WIPHY 1
#define NERO_NL80211_ATTR_WIPHY_NAME 2
#define NERO_NL80211_ATTR_IFINDEX 3
#define NERO_NL80211_ATTR_IFTYPE 5
#define NERO_NL80211_ATTR_MAC 6
#define NERO_NL80211_ATTR_WIPHY_BANDS 22
#define NERO_NL80211_ATTR_SUPPORTED_IFTYPES 32
#define NERO_NL80211_ATTR_REG_ALPHA2 33

#define NERO_NL80211_BAND_ATTR_FREQS 1
#define NERO_NL80211_FREQUENCY_ATTR_FREQ 1
#define NERO_NL80211_FREQUENCY_ATTR_DISABLED 2
#define NERO_NL80211_FREQUENCY_ATTR_NO_IR 3

#define NERO_NL80211_BAND_INDEX_2GHZ 0
#define NERO_NL80211_BAND_INDEX_5GHZ 1
#define NERO_NL80211_BAND_INDEX_60GHZ 2
#define NERO_NL80211_BAND_INDEX_6GHZ 3

struct nero_nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct nero_nlmsgerr {
    int32_t error;
    struct nero_nlmsghdr msg;
};

struct nero_genlmsghdr {
    uint8_t cmd;
    uint8_t version;
    uint16_t reserved;
};

struct nero_nlattr {
    uint16_t nla_len;
    uint16_t nla_type;
};

struct nero_sockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

_Static_assert(sizeof(struct nero_nlmsghdr) == 16, "netlink header layout");
_Static_assert(sizeof(struct nero_genlmsghdr) == 4, "generic netlink header layout");
_Static_assert(sizeof(struct nero_nlattr) == 4, "netlink attribute layout");
_Static_assert(sizeof(struct nero_sockaddr_nl) == 12, "netlink socket address layout");

#if defined(NERO_NL80211_TEST)
static void *test_context;
static nero_nl80211_exchange_fn test_exchange;
static nero_nl80211_ifindex_fn test_ifindex;
#endif
/* Keep protocol fixtures independent of the host's network adapters. */
static unsigned lookup_interface(const char *name)
{
#if defined(NERO_NL80211_TEST)
    if (test_ifindex != NULL)
        return test_ifindex(name);
#endif
#if defined(_WIN32)
    (void)name;
    return 0;
#else
    return if_nametoindex(name);
#endif
}

static int cached_family_id = -1;
static uint32_t next_sequence = 0;

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static void copy_text(char *out, size_t out_len, const char *value)
{
    if (out == NULL || out_len == 0)
        return;
    if (value == NULL) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%s", value);
}

static int copy_attr_text(const struct nero_nlattr *attr, char *out, size_t out_len)
{
    if (attr == NULL || out == NULL || out_len == 0)
        return -1;
    size_t length = attr->nla_len - NERO_NLA_HDRLEN;
    const char *text = (const char *)((const uint8_t *)attr + NERO_NLA_HDRLEN);
    size_t used = 0;
    while (used < length && text[used] != '\0' && used + 1 < out_len) {
        out[used] = text[used];
        ++used;
    }
    out[used] = '\0';
    return 0;
}

static uint32_t attr_u32(const struct nero_nlattr *attr)
{
    uint32_t value = 0;
    if (attr != NULL && attr->nla_len >= NERO_NLA_HDRLEN + sizeof(uint32_t))
        memcpy(&value, (const uint8_t *)attr + NERO_NLA_HDRLEN, sizeof(value));
    return value;
}

static int attr_ok(const struct nero_nlattr *attr, size_t remaining)
{
    return attr != NULL && remaining >= NERO_NLA_HDRLEN && attr->nla_len >= NERO_NLA_HDRLEN &&
           attr->nla_len <= remaining;
}

static const struct nero_nlattr *attr_next(const struct nero_nlattr *attr, size_t *remaining)
{
    size_t step = NERO_NLA_ALIGN(attr->nla_len);
    if (step > *remaining)
        return NULL;
    *remaining -= step;
    return (const struct nero_nlattr *)((const uint8_t *)attr + step);
}

static const struct nero_nlattr *attr_find(const void *data, size_t length, uint16_t type)
{
    const struct nero_nlattr *attr = data;
    size_t remaining = length;
    while (attr_ok(attr, remaining)) {
        if ((attr->nla_type & NERO_NLA_TYPE_MASK) == type)
            return attr;
        attr = attr_next(attr, &remaining);
    }
    return NULL;
}

static void attr_bounds(const struct nero_nlattr *attr, const void **data, size_t *length)
{
    *data = (const uint8_t *)attr + NERO_NLA_HDRLEN;
    *length = attr->nla_len - NERO_NLA_HDRLEN;
}

typedef struct {
    uint8_t buffer[4096];
    size_t length;
    uint32_t sequence;
} nero_nl_message;

static void message_begin(nero_nl_message *message, uint16_t type, uint16_t flags, uint8_t cmd)
{
    memset(message, 0, sizeof(*message));
    struct nero_nlmsghdr *header = (struct nero_nlmsghdr *)message->buffer;
    header->nlmsg_type = type;
    header->nlmsg_flags = (uint16_t)(flags | NERO_NLM_F_REQUEST);
    header->nlmsg_seq = ++next_sequence;
    if (header->nlmsg_seq == 0)
        header->nlmsg_seq = ++next_sequence;
    header->nlmsg_pid = 0;
    message->sequence = header->nlmsg_seq;
    struct nero_genlmsghdr *generic =
        (struct nero_genlmsghdr *)(message->buffer + NERO_NLMSG_HDRLEN);
    generic->cmd = cmd;
    generic->version = 1;
    message->length = NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN;
}

static int message_put(nero_nl_message *message, uint16_t type, const void *data, size_t length)
{
    size_t needed = NERO_NLA_HDRLEN + length;
    size_t aligned = NERO_NLA_ALIGN(needed);
    if (message->length + aligned > sizeof(message->buffer))
        return -1;
    struct nero_nlattr *attr = (struct nero_nlattr *)(message->buffer + message->length);
    attr->nla_len = (uint16_t)needed;
    attr->nla_type = type;
    if (length > 0)
        memcpy(message->buffer + message->length + NERO_NLA_HDRLEN, data, length);
    if (aligned > needed)
        memset(message->buffer + message->length + needed, 0, aligned - needed);
    message->length += aligned;
    return 0;
}

static int message_put_u32(nero_nl_message *message, uint16_t type, uint32_t value)
{
    return message_put(message, type, &value, sizeof(value));
}

static int message_put_string(nero_nl_message *message, uint16_t type, const char *text)
{
    return message_put(message, type, text, strlen(text) + 1);
}

static void message_finish(nero_nl_message *message)
{
    struct nero_nlmsghdr *header = (struct nero_nlmsghdr *)message->buffer;
    header->nlmsg_len = (uint32_t)message->length;
}

#if !defined(_WIN32)
static int netlink_socket(char *error, size_t error_len)
{
    int fd = socket(NERO_AF_NETLINK, SOCK_RAW, NERO_NETLINK_GENERIC);
    if (fd < 0) {
        char message[128];
        snprintf(message, sizeof(message), "nl80211 netlink socket unavailable: %s",
                 strerror(errno));
        set_error(error, error_len, message);
        return -1;
    }
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct nero_sockaddr_nl local;
    memset(&local, 0, sizeof(local));
    local.nl_family = NERO_AF_NETLINK;
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        char message[128];
        snprintf(message, sizeof(message), "nl80211 netlink bind failed: %s", strerror(errno));
        set_error(error, error_len, message);
        close(fd);
        return -1;
    }
    return fd;
}
#endif

static int transport_exchange(const void *request, size_t request_len, void *reply,
                              size_t reply_capacity, size_t *reply_len, char *error,
                              size_t error_len)
{
#if defined(NERO_NL80211_TEST)
    if (test_exchange != NULL) {
        *reply_len = 0;
        return test_exchange(test_context, request, request_len, reply, reply_capacity, reply_len,
                             error, error_len);
    }
#endif
#if defined(_WIN32)
    (void)request;
    (void)request_len;
    (void)reply;
    (void)reply_capacity;
    (void)reply_len;
    set_error(error, error_len, "nl80211 is only available on Linux");
    return -1;
#else
    int fd = netlink_socket(error, error_len);
    if (fd < 0)
        return -1;
    struct nero_sockaddr_nl destination;
    memset(&destination, 0, sizeof(destination));
    destination.nl_family = NERO_AF_NETLINK;
    ssize_t sent = sendto(fd, request, request_len, 0, (const struct sockaddr *)&destination,
                          sizeof(destination));
    if (sent < 0 || (size_t)sent != request_len) {
        char message[128];
        snprintf(message, sizeof(message), "nl80211 send failed: %s", strerror(errno));
        set_error(error, error_len, message);
        close(fd);
        return -1;
    }
    ssize_t received = recv(fd, reply, reply_capacity, 0);
    if (received <= 0) {
        char message[128];
        snprintf(message, sizeof(message), "nl80211 receive failed: %s",
                 received < 0 ? strerror(errno) : "connection closed");
        set_error(error, error_len, message);
        close(fd);
        return -1;
    }
    *reply_len = (size_t)received;
    close(fd);
    return 0;
#endif
}

/* Returns 0 and sets *message when a data reply arrived. Sets *ack for an ACK
 * or DONE message. Returns -1 for kernel errors, truncated replies or a missing
 * matching sequence number. */
static int reply_message(const uint8_t *reply, size_t reply_len, uint32_t sequence,
                         const struct nero_nlmsghdr **message, int *ack, char *error,
                         size_t error_len)
{
    size_t offset = 0;
    if (message != NULL)
        *message = NULL;
    if (ack != NULL)
        *ack = 0;
    while (offset + NERO_NLMSG_HDRLEN <= reply_len) {
        const struct nero_nlmsghdr *header =
            (const struct nero_nlmsghdr *)(reply + offset);
        if (header->nlmsg_len < NERO_NLMSG_HDRLEN || header->nlmsg_len > reply_len - offset) {
            set_error(error, error_len, "nl80211 reply is truncated");
            return -1;
        }
        if (header->nlmsg_seq != sequence) {
            offset += NERO_NLMSG_ALIGN(header->nlmsg_len);
            continue;
        }
        if (header->nlmsg_type == NERO_NLMSG_ERROR) {
            if (header->nlmsg_len < NERO_NLMSG_HDRLEN + sizeof(int32_t)) {
                set_error(error, error_len, "nl80211 error reply is truncated");
                return -1;
            }
            int32_t code = 0;
            memcpy(&code, reply + offset + NERO_NLMSG_HDRLEN, sizeof(code));
            if (code == 0) {
                if (ack != NULL)
                    *ack = 1;
                return 0;
            }
            char text[192];
            snprintf(text, sizeof(text), "nl80211: %s (%d)", strerror(-code), (int)code);
            set_error(error, error_len, text);
            return -1;
        }
        if (header->nlmsg_type == NERO_NLMSG_DONE) {
            if (ack != NULL)
                *ack = 1;
            return 0;
        }
        if (header->nlmsg_type == NERO_NLMSG_NOOP) {
            offset += NERO_NLMSG_ALIGN(header->nlmsg_len);
            continue;
        }
        if (message != NULL)
            *message = header;
        return 0;
    }
    set_error(error, error_len, "nl80211 reply did not match the request sequence");
    return -1;
}

static int resolve_family(char *error, size_t error_len)
{
    if (cached_family_id >= 0)
        return cached_family_id;
    nero_nl_message request;
    message_begin(&request, NERO_GENL_ID_CTRL, 0, NERO_GENL_CMD_GETFAMILY);
    if (message_put_string(&request, NERO_CTRL_ATTR_FAMILY_NAME, "nl80211") != 0) {
        set_error(error, error_len, "nl80211 family request is too large");
        return -1;
    }
    message_finish(&request);
    uint8_t reply[8192];
    size_t reply_len = 0;
    if (transport_exchange(request.buffer, request.length, reply, sizeof(reply), &reply_len, error,
                           error_len) != 0) {
        return -1;
    }
    const struct nero_nlmsghdr *header = NULL;
    int ack = 0;
    if (reply_message(reply, reply_len, request.sequence, &header, &ack, error, error_len) != 0)
        return -1;
    if (ack || header == NULL) {
        set_error(error, error_len, "nl80211 family lookup returned no data");
        return -1;
    }
    if (header->nlmsg_len < NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN) {
        set_error(error, error_len, "nl80211 family reply is truncated");
        return -1;
    }
    const uint8_t *attributes = (const uint8_t *)header + NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN;
    size_t attributes_len = header->nlmsg_len - NERO_NLMSG_HDRLEN - NERO_GENL_HDRLEN;
    const struct nero_nlattr *family = attr_find(attributes, attributes_len,
                                                  NERO_CTRL_ATTR_FAMILY_ID);
    if (family == NULL || family->nla_len < NERO_NLA_HDRLEN + sizeof(uint16_t)) {
        set_error(error, error_len, "nl80211 family id is missing");
        return -1;
    }
    uint16_t id = 0;
    memcpy(&id, (const uint8_t *)family + NERO_NLA_HDRLEN, sizeof(id));
    if (id == 0) {
        set_error(error, error_len, "nl80211 family id is zero");
        return -1;
    }
    cached_family_id = id;
    return cached_family_id;
}

static int request_reply(const nero_nl_message *request, uint8_t *reply, size_t reply_capacity,
                         size_t *reply_len, char *error, size_t error_len)
{
    return transport_exchange(request->buffer, request->length, reply, reply_capacity, reply_len,
                              error, error_len);
}

static void parse_interface(const uint8_t *attributes, size_t attributes_len, nero_nl80211_info *out)
{
    const struct nero_nlattr *attr = attr_find(attributes, attributes_len,
                                               NERO_NL80211_ATTR_IFINDEX);
    if (attr != NULL)
        out->ifindex = (int)attr_u32(attr);
    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_WIPHY);
    if (attr != NULL)
        out->wiphy = (int)attr_u32(attr);
    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_IFTYPE);
    if (attr != NULL) {
        out->iftype = (int)attr_u32(attr);
        out->has_iftype = 1;
    }
    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_MAC);
    if (attr != NULL && attr->nla_len == NERO_NLA_HDRLEN + 6) {
        const uint8_t *mac = (const uint8_t *)attr + NERO_NLA_HDRLEN;
        snprintf(out->mac, sizeof(out->mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }
}

static unsigned band_bit_for_index(unsigned index)
{
    switch (index) {
    case NERO_NL80211_BAND_INDEX_2GHZ:
        return NERO_NL80211_BAND_2GHZ;
    case NERO_NL80211_BAND_INDEX_5GHZ:
        return NERO_NL80211_BAND_5GHZ;
    case NERO_NL80211_BAND_INDEX_6GHZ:
        return NERO_NL80211_BAND_6GHZ;
    case NERO_NL80211_BAND_INDEX_60GHZ:
        return NERO_NL80211_BAND_60GHZ;
    default:
        return 0;
    }
}

static unsigned band_bit_for_frequency(uint32_t frequency)
{
    if (frequency >= 2400 && frequency < 2500)
        return NERO_NL80211_BAND_2GHZ;
    if (frequency >= 4900 && frequency < 5925)
        return NERO_NL80211_BAND_5GHZ;
    if (frequency >= 5925 && frequency < 7125)
        return NERO_NL80211_BAND_6GHZ;
    if (frequency >= 57000 && frequency < 71000)
        return NERO_NL80211_BAND_60GHZ;
    return 0;
}

static void parse_frequencies(const uint8_t *entries, size_t entries_len, unsigned band,
                              nero_nl80211_info *out)
{
    const struct nero_nlattr *entry = (const struct nero_nlattr *)entries;
    size_t remaining = entries_len;
    while (attr_ok(entry, remaining)) {
        const void *nested = NULL;
        size_t nested_len = 0;
        attr_bounds(entry, &nested, &nested_len);
        const struct nero_nlattr *frequency =
            attr_find(nested, nested_len, NERO_NL80211_FREQUENCY_ATTR_FREQ);
        if (frequency != NULL) {
            uint32_t value = attr_u32(frequency);
            unsigned resolved = band != 0 ? band : band_bit_for_frequency(value);
            if (resolved != 0)
                out->bands |= resolved;
            if (attr_find(nested, nested_len, NERO_NL80211_FREQUENCY_ATTR_DISABLED) != NULL)
                ++out->disabled_channel_count;
            else
                ++out->channel_count;
            if (attr_find(nested, nested_len, NERO_NL80211_FREQUENCY_ATTR_NO_IR) != NULL)
                ++out->no_ir_channel_count;
        }
        entry = attr_next(entry, &remaining);
    }
}

static void parse_bands(const uint8_t *attributes, size_t attributes_len, nero_nl80211_info *out)
{
    const struct nero_nlattr *band = (const struct nero_nlattr *)attributes;
    size_t remaining = attributes_len;
    while (attr_ok(band, remaining)) {
        unsigned index = band->nla_type & NERO_NLA_TYPE_MASK;
        unsigned bit = band_bit_for_index(index);
        if (bit != 0)
            out->bands |= bit;
        const void *nested = NULL;
        size_t nested_len = 0;
        attr_bounds(band, &nested, &nested_len);
        const struct nero_nlattr *freqs =
            attr_find(nested, nested_len, NERO_NL80211_BAND_ATTR_FREQS);
        if (freqs != NULL) {
            const void *freq_data = NULL;
            size_t freq_len = 0;
            attr_bounds(freqs, &freq_data, &freq_len);
            parse_frequencies(freq_data, freq_len, bit, out);
        }
        band = attr_next(band, &remaining);
    }
}

static void parse_supported_iftypes(const uint8_t *attributes, size_t attributes_len,
                                    nero_nl80211_info *out)
{
    const struct nero_nlattr *attr = (const struct nero_nlattr *)attributes;
    size_t remaining = attributes_len;
    out->has_supported_iftypes = 1;
    while (attr_ok(attr, remaining)) {
        unsigned type = attr->nla_type & NERO_NLA_TYPE_MASK;
        if (type < 32)
            out->supported_iftypes |= 1u << type;
        attr = attr_next(attr, &remaining);
    }
    out->monitor_supported = (out->supported_iftypes & (1u << NERO_NL80211_IFTYPE_MONITOR)) != 0;
}

static void parse_wiphy(const uint8_t *attributes, size_t attributes_len, nero_nl80211_info *out)
{
    const struct nero_nlattr *attr = attr_find(attributes, attributes_len,
                                               NERO_NL80211_ATTR_WIPHY_NAME);
    if (attr != NULL)
        (void)copy_attr_text(attr, out->phy_name, sizeof(out->phy_name));
    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_REG_ALPHA2);
    if (attr != NULL)
        (void)copy_attr_text(attr, out->regulatory, sizeof(out->regulatory));

    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_SUPPORTED_IFTYPES);
    if (attr != NULL && attr->nla_len > NERO_NLA_HDRLEN) {
        const void *nested = NULL;
        size_t nested_len = 0;
        attr_bounds(attr, &nested, &nested_len);
        parse_supported_iftypes(nested, nested_len, out);
    }
    attr = attr_find(attributes, attributes_len, NERO_NL80211_ATTR_WIPHY_BANDS);
    if (attr != NULL && attr->nla_len > NERO_NLA_HDRLEN) {
        const void *nested = NULL;
        size_t nested_len = 0;
        attr_bounds(attr, &nested, &nested_len);
        parse_bands(nested, nested_len, out);
    }
}

static void fill_driver(const char *interface, char *out, size_t out_len)
{
#if defined(__linux__)
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", interface);
    char target[256];
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0)
        return;
    target[length] = '\0';
    const char *base = strrchr(target, '/');
    copy_text(out, out_len, base != NULL ? base + 1 : target);
#else
    (void)interface;
    (void)out;
    (void)out_len;
#endif
}

#if defined(NERO_NL80211_TEST)
void nero_nl80211_test_set_exchange(void *context, nero_nl80211_exchange_fn exchange)
{
    test_context = context;
    test_exchange = exchange;
    cached_family_id = -1;
    next_sequence = 0;
}

void nero_nl80211_test_set_ifindex(nero_nl80211_ifindex_fn lookup)
{
    test_ifindex = lookup;
}

void nero_nl80211_test_reset(void)
{
    nero_nl80211_test_set_exchange(NULL, NULL);
    test_ifindex = NULL;
}
#endif

#endif /* !NERO_NL80211_OFFLINE */

int nero_nl80211_available(void)
{
#if defined(NERO_NL80211_OFFLINE)
    return 0;
#elif defined(_WIN32)
    return 0;
#else
    int fd = socket(NERO_AF_NETLINK, SOCK_RAW, NERO_NETLINK_GENERIC);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
#endif
}

const char *nero_nl80211_iftype_name(int iftype)
{
    switch (iftype) {
    case NERO_NL80211_IFTYPE_UNSPECIFIED:
        return "unspecified";
    case NERO_NL80211_IFTYPE_ADHOC:
        return "ad-hoc";
    case NERO_NL80211_IFTYPE_STATION:
        return "station/managed";
    case NERO_NL80211_IFTYPE_AP:
        return "access point";
    case NERO_NL80211_IFTYPE_AP_VLAN:
        return "AP VLAN";
    case NERO_NL80211_IFTYPE_WDS:
        return "WDS";
    case NERO_NL80211_IFTYPE_MONITOR:
        return "monitor";
    case NERO_NL80211_IFTYPE_MESH_POINT:
        return "mesh point";
    case NERO_NL80211_IFTYPE_P2P_CLIENT:
        return "P2P client";
    case NERO_NL80211_IFTYPE_P2P_GO:
        return "P2P group owner";
    case NERO_NL80211_IFTYPE_P2P_DEVICE:
        return "P2P device";
    case NERO_NL80211_IFTYPE_OCB:
        return "OCB";
    case NERO_NL80211_IFTYPE_NAN:
        return "NAN";
    default:
        return "unknown";
    }
}

const char *nero_nl80211_band_summary(unsigned bands, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0)
        return "";
    buffer[0] = '\0';
    struct {
        unsigned bit;
        const char *name;
    } names[] = {{NERO_NL80211_BAND_2GHZ, "2.4 GHz"},
                 {NERO_NL80211_BAND_5GHZ, "5 GHz"},
                 {NERO_NL80211_BAND_6GHZ, "6 GHz"},
                 {NERO_NL80211_BAND_60GHZ, "60 GHz"}};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((bands & names[i].bit) == 0)
            continue;
        if (buffer[0] != '\0')
            strncat(buffer, ", ", buffer_len - strlen(buffer) - 1);
        strncat(buffer, names[i].name, buffer_len - strlen(buffer) - 1);
    }
    if (buffer[0] == '\0')
        snprintf(buffer, buffer_len, "unknown");
    return buffer;
}

#if defined(NERO_NL80211_OFFLINE)
int nero_nl80211_get_info(const char *interface, nero_nl80211_info *out, char *error,
                          size_t error_len)
{
    (void)interface;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s", "nl80211 is unavailable in this offline build");
    return -1;
}

int nero_nl80211_set_type(const char *interface, int iftype, nero_nl80211_info *verified,
                          char *error, size_t error_len)
{
    (void)interface;
    (void)iftype;
    (void)verified;
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s",
                 "nl80211 interface control is unavailable in this offline build");
    return -1;
}

int nero_nl80211_set_frequency(const char *interface, int freq_mhz, char *error, size_t error_len)
{
    (void)interface;
    (void)freq_mhz;
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s",
                 "nl80211 channel control is unavailable in this offline build");
    return -1;
}
#else
int nero_nl80211_get_info(const char *interface, nero_nl80211_info *out, char *error,
                          size_t error_len)
{
    if (interface == NULL || interface[0] == '\0' || out == NULL) {
        set_error(error, error_len, "invalid nl80211 interface argument");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    copy_text(out->interface, sizeof(out->interface), interface);

#if defined(NERO_NL80211_TEST)
    int mocked = test_exchange != NULL;
#else
    int mocked = 0;
#endif
    if (!mocked && !nero_nl80211_available()) {
        set_error(error, error_len, "nl80211 is only available on Linux");
        return -1;
    }

    unsigned ifindex = lookup_interface(interface);
    if (ifindex == 0) {
        char message[192];
        snprintf(message, sizeof(message), "interface '%s' does not exist", interface);
        set_error(error, error_len, message);
        return -1;
    }
    out->ifindex = (int)ifindex;
    out->available = 1;

    int family = resolve_family(error, error_len);
    if (family < 0)
        return -1;

    uint8_t reply[8192];
    size_t reply_len = 0;
    nero_nl_message request;
    message_begin(&request, (uint16_t)family, 0, NERO_NL80211_CMD_GET_INTERFACE);
    if (message_put_u32(&request, NERO_NL80211_ATTR_IFINDEX, ifindex) != 0) {
        set_error(error, error_len, "nl80211 interface request is too large");
        return -1;
    }
    message_finish(&request);
    if (request_reply(&request, reply, sizeof(reply), &reply_len, error, error_len) != 0)
        return -1;
    const struct nero_nlmsghdr *header = NULL;
    int ack = 0;
    if (reply_message(reply, reply_len, request.sequence, &header, &ack, error, error_len) != 0)
        return -1;
    if (ack || header == NULL) {
        set_error(error, error_len, "nl80211 interface query returned no data");
        return -1;
    }
    if (header->nlmsg_len < NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN) {
        set_error(error, error_len, "nl80211 interface reply is truncated");
        return -1;
    }
    parse_interface((const uint8_t *)header + NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN,
                    header->nlmsg_len - NERO_NLMSG_HDRLEN - NERO_GENL_HDRLEN, out);

    message_begin(&request, (uint16_t)family, 0, NERO_NL80211_CMD_GET_WIPHY);
    (void)message_put_u32(&request, NERO_NL80211_ATTR_IFINDEX, ifindex);
    message_finish(&request);
    reply_len = 0;
    if (request_reply(&request, reply, sizeof(reply), &reply_len, error, error_len) != 0)
        return -1;
    header = NULL;
    ack = 0;
    if (reply_message(reply, reply_len, request.sequence, &header, &ack, error, error_len) != 0)
        return -1;
    if (ack || header == NULL) {
        set_error(error, error_len, "nl80211 wiphy query returned no data");
        return -1;
    }
    if (header->nlmsg_len < NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN) {
        set_error(error, error_len, "nl80211 wiphy reply is truncated");
        return -1;
    }
    parse_wiphy((const uint8_t *)header + NERO_NLMSG_HDRLEN + NERO_GENL_HDRLEN,
                header->nlmsg_len - NERO_NLMSG_HDRLEN - NERO_GENL_HDRLEN, out);
    fill_driver(interface, out->driver, sizeof(out->driver));
    return 0;
}

int nero_nl80211_set_type(const char *interface, int iftype, nero_nl80211_info *verified,
                          char *error, size_t error_len)
{
    if (interface == NULL || interface[0] == '\0') {
        set_error(error, error_len, "invalid nl80211 interface argument");
        return -1;
    }
    nero_nl80211_info before;
    if (nero_nl80211_get_info(interface, &before, error, error_len) != 0)
        return -1;
    if (before.has_iftype && before.iftype == iftype) {
        if (verified != NULL)
            *verified = before;
        return 0;
    }
    if (before.has_supported_iftypes &&
        (before.supported_iftypes & (1u << (unsigned)iftype)) == 0) {
        char message[192];
        snprintf(message, sizeof(message), "interface type '%s' is not supported by %s",
                 nero_nl80211_iftype_name(iftype), before.phy_name[0] != '\0' ? before.phy_name
                                                                              : "this wiphy");
        set_error(error, error_len, message);
        return -1;
    }
    if (!before.has_supported_iftypes && iftype == NERO_NL80211_IFTYPE_MONITOR) {
        set_error(error, error_len,
                  "supported interface types are unavailable; refusing an unverified monitor "
                  "transition");
        return -1;
    }

    int family = resolve_family(error, error_len);
    if (family < 0)
        return -1;
    nero_nl_message request;
    message_begin(&request, (uint16_t)family, NERO_NLM_F_ACK, NERO_NL80211_CMD_SET_INTERFACE);
    if (message_put_u32(&request, NERO_NL80211_ATTR_IFINDEX, (uint32_t)before.ifindex) != 0 ||
        message_put_u32(&request, NERO_NL80211_ATTR_IFTYPE, (uint32_t)iftype) != 0) {
        set_error(error, error_len, "nl80211 type request is too large");
        return -1;
    }
    message_finish(&request);
    uint8_t reply[8192];
    size_t reply_len = 0;
    if (request_reply(&request, reply, sizeof(reply), &reply_len, error, error_len) != 0)
        return -1;
    const struct nero_nlmsghdr *header = NULL;
    int ack = 0;
    if (reply_message(reply, reply_len, request.sequence, &header, &ack, error, error_len) != 0)
        return -1;
    if (!ack && header == NULL) {
        set_error(error, error_len, "nl80211 type change returned no acknowledgement");
        return -1;
    }

    nero_nl80211_info after;
    if (nero_nl80211_get_info(interface, &after, error, error_len) != 0) {
        char message[256];
        snprintf(message, sizeof(message),
                 "type change could not be verified because the interface disappeared or could "
                 "not be re-read: %s",
                 error != NULL && error[0] != '\0' ? error : "unknown error");
        set_error(error, error_len, message);
        return -1;
    }
    if (!after.has_iftype || after.iftype != iftype) {
        char message[256];
        snprintf(message, sizeof(message),
                 "type change verification failed: requested '%s', kernel reports '%s'",
                 nero_nl80211_iftype_name(iftype),
                 after.has_iftype ? nero_nl80211_iftype_name(after.iftype) : "unknown");
        set_error(error, error_len, message);
        return -1;
    }
    if (verified != NULL)
        *verified = after;
    return 0;
}

int nero_nl80211_set_frequency(const char *interface, int freq_mhz, char *error, size_t error_len)
{
    if (interface == NULL || interface[0] == '\0') {
        set_error(error, error_len, "invalid nl80211 interface argument");
        return -1;
    }
    if (freq_mhz < 2000 || freq_mhz > 7250) {
        set_error(error, error_len, "frequency is outside the supported 2.4/5/6 GHz range");
        return -1;
    }

#if defined(NERO_NL80211_TEST)
    int mocked = test_exchange != NULL;
#else
    int mocked = 0;
#endif
    if (!mocked && !nero_nl80211_available()) {
        set_error(error, error_len, "nl80211 is only available on Linux");
        return -1;
    }

    unsigned ifindex = lookup_interface(interface);
    if (ifindex == 0) {
        char message[192];
        snprintf(message, sizeof(message), "interface '%s' does not exist", interface);
        set_error(error, error_len, message);
        return -1;
    }

    int family = resolve_family(error, error_len);
    if (family < 0)
        return -1;

    nero_nl_message request;
    message_begin(&request, (uint16_t)family, NERO_NLM_F_ACK, NERO_NL80211_CMD_SET_CHANNEL);
    if (message_put_u32(&request, NERO_NL80211_ATTR_IFINDEX, ifindex) != 0 ||
        message_put_u32(&request, NERO_NL80211_ATTR_WIPHY_FREQ, (uint32_t)freq_mhz) != 0 ||
        message_put_u32(&request, NERO_NL80211_ATTR_WIPHY_CHANNEL_TYPE,
                        NERO_NL80211_CHAN_NO_HT) != 0) {
        set_error(error, error_len, "nl80211 channel request is too large");
        return -1;
    }
    message_finish(&request);

    uint8_t reply[8192];
    size_t reply_len = 0;
    if (request_reply(&request, reply, sizeof(reply), &reply_len, error, error_len) != 0)
        return -1;
    const struct nero_nlmsghdr *header = NULL;
    int ack = 0;
    if (reply_message(reply, reply_len, request.sequence, &header, &ack, error, error_len) != 0)
        return -1;
    if (!ack && header == NULL) {
        set_error(error, error_len, "nl80211 channel change returned no acknowledgement");
        return -1;
    }
    return 0;
}
#endif /* !NERO_NL80211_OFFLINE */
