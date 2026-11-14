/* Deterministic mock-transport tests for the nl80211 protocol core.
 *
 * These tests exercise request serialization, attribute parsing, error
 * propagation and post-change verification without touching a real kernel.
 * Hardware/driver validation still requires Linux and is tracked separately. */

#include "nero/wifi_nl80211.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <net/if.h>
#endif

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define NLMSG_HDRLEN 16
#define GENL_HDRLEN 4
#define NLA_HDRLEN 4
#define NLA_ALIGNTO 4
#define NLA_ALIGN(length) (((length) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_TYPE_MASK 0x3fffu
#define NLA_F_NESTED 0x8000u

#define GENL_ID_CTRL 16
#define CMD_GETFAMILY 3
#define CMD_GET_WIPHY 1
#define CMD_GET_INTERFACE 5
#define CMD_SET_INTERFACE 6
#define CMD_SET_CHANNEL 65
#define ATTR_WIPHY_FREQ 38
#define CTRL_ATTR_FAMILY_ID 1
#define ATTR_WIPHY 1
#define ATTR_WIPHY_NAME 2
#define ATTR_IFINDEX 3
#define ATTR_IFTYPE 5
#define ATTR_MAC 6
#define ATTR_WIPHY_BANDS 22
#define ATTR_SUPPORTED_IFTYPES 32
#define ATTR_REG_ALPHA2 33
#define BAND_ATTR_FREQS 1
#define FREQ_ATTR_FREQ 1
#define FREQ_ATTR_DISABLED 2
#define FREQ_ATTR_NO_IR 3
#define FAMILY_ID 42

struct test_nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct test_nlattr {
    uint16_t nla_len;
    uint16_t nla_type;
};

typedef struct {
    uint8_t buffer[8192];
    size_t length;
} builder;

typedef struct {
    int iftype;
    int wiphy;
    unsigned supported_iftypes;
    int omit_supported;
    int omit_bands;
    int refuse_verify;
    int deny_set;
    int malformed;
    int fail_family;
    int ctrl_requests;
    int get_requests;
    int wiphy_requests;
    int set_requests;
    uint32_t last_set_ifindex;
    uint32_t last_set_iftype;
    int channel_requests;
    int deny_channel;
    uint32_t last_set_freq;
} mock;

static void builder_init(builder *b)
{
    memset(b, 0, sizeof(*b));
}

static void builder_header(builder *b, uint16_t type, uint16_t flags, uint32_t sequence,
                           uint8_t command)
{
    b->length = NLMSG_HDRLEN + GENL_HDRLEN;
    struct test_nlmsghdr *header = (struct test_nlmsghdr *)b->buffer;
    header->nlmsg_len = (uint32_t)b->length;
    header->nlmsg_type = type;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = sequence;
    header->nlmsg_pid = 0;
    b->buffer[NLMSG_HDRLEN] = command;
    b->buffer[NLMSG_HDRLEN + 1] = 1;
}

static size_t put_attr(builder *b, uint16_t type, const void *data, size_t length)
{
    size_t offset = b->length;
    size_t needed = NLA_HDRLEN + length;
    size_t aligned = NLA_ALIGN(needed);
    if (offset + aligned > sizeof(b->buffer))
        abort();
    struct test_nlattr *attr = (struct test_nlattr *)(b->buffer + offset);
    attr->nla_len = (uint16_t)needed;
    attr->nla_type = type;
    if (length > 0)
        memcpy(b->buffer + offset + NLA_HDRLEN, data, length);
    if (aligned > needed)
        memset(b->buffer + offset + needed, 0, aligned - needed);
    b->length += aligned;
    return offset;
}

static void put_u32(builder *b, uint16_t type, uint32_t value)
{
    (void)put_attr(b, type, &value, sizeof(value));
}

static void put_u16(builder *b, uint16_t type, uint16_t value)
{
    (void)put_attr(b, type, &value, sizeof(value));
}

static void put_string(builder *b, uint16_t type, const char *value)
{
    (void)put_attr(b, type, value, strlen(value) + 1);
}

static void put_empty(builder *b, uint16_t type)
{
    (void)put_attr(b, type, NULL, 0);
}

static size_t nested_begin(builder *b, uint16_t type)
{
    return put_attr(b, (uint16_t)(type | NLA_F_NESTED), NULL, 0);
}

static void nested_end(builder *b, size_t offset)
{
    struct test_nlattr *attr = (struct test_nlattr *)(b->buffer + offset);
    attr->nla_len = (uint16_t)(b->length - offset);
}

static void frequency(builder *b, unsigned index, uint32_t value, int disabled, int no_ir)
{
    size_t entry = nested_begin(b, (uint16_t)index);
    put_u32(b, FREQ_ATTR_FREQ, value);
    if (disabled)
        put_empty(b, FREQ_ATTR_DISABLED);
    if (no_ir)
        put_empty(b, FREQ_ATTR_NO_IR);
    nested_end(b, entry);
}

static void band(builder *b, unsigned index, const uint32_t *frequencies, size_t count,
                 const int *disabled, const int *no_ir)
{
    size_t band_offset = nested_begin(b, (uint16_t)index);
    size_t freqs = nested_begin(b, BAND_ATTR_FREQS);
    for (size_t i = 0; i < count; ++i)
        frequency(b, (unsigned)i, frequencies[i], disabled[i], no_ir[i]);
    nested_end(b, freqs);
    nested_end(b, band_offset);
}

static void finish(builder *b)
{
    struct test_nlmsghdr *header = (struct test_nlmsghdr *)b->buffer;
    header->nlmsg_len = (uint32_t)b->length;
}

static const struct test_nlattr *request_attr(const uint8_t *request, size_t request_len,
                                              uint16_t type)
{
    if (request_len < NLMSG_HDRLEN + GENL_HDRLEN)
        return NULL;
    const struct test_nlattr *attr =
        (const struct test_nlattr *)(request + NLMSG_HDRLEN + GENL_HDRLEN);
    size_t remaining = request_len - NLMSG_HDRLEN - GENL_HDRLEN;
    while (remaining >= NLA_HDRLEN) {
        if (attr->nla_len < NLA_HDRLEN || attr->nla_len > remaining)
            return NULL;
        if ((attr->nla_type & NLA_TYPE_MASK) == type)
            return attr;
        size_t step = NLA_ALIGN(attr->nla_len);
        if (step > remaining)
            return NULL;
        remaining -= step;
        attr = (const struct test_nlattr *)((const uint8_t *)attr + step);
    }
    return NULL;
}

static uint32_t request_u32(const uint8_t *request, size_t request_len, uint16_t type)
{
    const struct test_nlattr *attr = request_attr(request, request_len, type);
    uint32_t value = 0;
    if (attr != NULL && attr->nla_len >= NLA_HDRLEN + sizeof(value))
        memcpy(&value, (const uint8_t *)attr + NLA_HDRLEN, sizeof(value));
    return value;
}

static int mock_exchange(void *context, const void *request, size_t request_len, void *reply,
                         size_t reply_capacity, size_t *reply_len, char *error, size_t error_len)
{
    mock *m = context;
    (void)error;
    (void)error_len;
    if (request_len < NLMSG_HDRLEN + GENL_HDRLEN || reply_capacity < 64) {
        fprintf(stderr, "mock: malformed request\n");
        return -1;
    }
    const struct test_nlmsghdr *header = (const struct test_nlmsghdr *)request;
    uint8_t command = ((const uint8_t *)request)[NLMSG_HDRLEN];
    builder b;
    builder_init(&b);

    if (m->malformed) {
        builder_header(&b, FAMILY_ID, 0, header->nlmsg_seq, command);
        finish(&b);
        struct test_nlmsghdr *bad = (struct test_nlmsghdr *)b.buffer;
        bad->nlmsg_len = (uint32_t)(b.length + 200);
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    if (header->nlmsg_type == GENL_ID_CTRL) {
        ++m->ctrl_requests;
        if (m->fail_family) {
            builder_init(&b);
            b.length = NLMSG_HDRLEN + 4 + 16;
            struct test_nlmsghdr *err = (struct test_nlmsghdr *)b.buffer;
            err->nlmsg_len = (uint32_t)b.length;
            err->nlmsg_type = 2;
            err->nlmsg_seq = header->nlmsg_seq;
            int32_t code = -EPROTONOSUPPORT;
            memcpy(b.buffer + NLMSG_HDRLEN, &code, sizeof(code));
            memcpy(reply, b.buffer, b.length);
            *reply_len = b.length;
            return 0;
        }
        if (command != CMD_GETFAMILY)
            return -1;
        builder_header(&b, GENL_ID_CTRL, 0, header->nlmsg_seq, CMD_GETFAMILY);
        put_u16(&b, CTRL_ATTR_FAMILY_ID, FAMILY_ID);
        finish(&b);
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    if (header->nlmsg_type != FAMILY_ID)
        return -1;

    if (command == CMD_GET_INTERFACE) {
        ++m->get_requests;
        uint32_t ifindex = request_u32(request, request_len, ATTR_IFINDEX);
        builder_header(&b, FAMILY_ID, 0, header->nlmsg_seq, CMD_GET_INTERFACE);
        put_u32(&b, ATTR_IFINDEX, ifindex);
        put_u32(&b, ATTR_WIPHY, (uint32_t)m->wiphy);
        put_u32(&b, ATTR_IFTYPE, (uint32_t)m->iftype);
        static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        put_attr(&b, ATTR_MAC, mac, sizeof(mac));
        finish(&b);
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    if (command == CMD_GET_WIPHY) {
        ++m->wiphy_requests;
        builder_header(&b, FAMILY_ID, 0, header->nlmsg_seq, CMD_GET_WIPHY);
        put_string(&b, ATTR_WIPHY_NAME, "phy0");
        put_string(&b, ATTR_REG_ALPHA2, "US");
        if (!m->omit_supported) {
            size_t supported = nested_begin(&b, ATTR_SUPPORTED_IFTYPES);
            for (int type = 1; type < 12; ++type) {
                if ((m->supported_iftypes & (1u << (unsigned)type)) != 0)
                    put_empty(&b, (uint16_t)type);
            }
            nested_end(&b, supported);
        }
        if (!m->omit_bands) {
            size_t bands = nested_begin(&b, ATTR_WIPHY_BANDS);
            static const uint32_t band24[] = {2412, 2437, 2462};
            static const int disabled24[] = {0, 1, 0};
            static const int noir24[] = {0, 0, 1};
            band(&b, 0, band24, 3, disabled24, noir24);
            static const uint32_t band5[] = {5180, 5200};
            static const int disabled5[] = {0, 1};
            static const int noir5[] = {0, 1};
            band(&b, 1, band5, 2, disabled5, noir5);
            static const uint32_t band6[] = {5955};
            static const int disabled6[] = {0};
            static const int noir6[] = {0};
            band(&b, 3, band6, 1, disabled6, noir6);
            nested_end(&b, bands);
        }
        finish(&b);
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    if (command == CMD_SET_INTERFACE) {
        ++m->set_requests;
        m->last_set_ifindex = request_u32(request, request_len, ATTR_IFINDEX);
        m->last_set_iftype = request_u32(request, request_len, ATTR_IFTYPE);
        builder_init(&b);
        b.length = NLMSG_HDRLEN + 4 + NLMSG_HDRLEN;
        struct test_nlmsghdr *err = (struct test_nlmsghdr *)b.buffer;
        err->nlmsg_len = (uint32_t)b.length;
        err->nlmsg_type = 2;
        err->nlmsg_flags = 0;
        err->nlmsg_seq = header->nlmsg_seq;
        int32_t code = m->deny_set ? -EPERM : 0;
        memcpy(b.buffer + NLMSG_HDRLEN, &code, sizeof(code));
        memcpy(b.buffer + NLMSG_HDRLEN + 4, request, NLMSG_HDRLEN);
        if (code == 0 && !m->refuse_verify)
            m->iftype = (int)m->last_set_iftype;
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    if (command == CMD_SET_CHANNEL) {
        ++m->channel_requests;
        m->last_set_ifindex = request_u32(request, request_len, ATTR_IFINDEX);
        m->last_set_freq = request_u32(request, request_len, ATTR_WIPHY_FREQ);
        builder_init(&b);
        b.length = NLMSG_HDRLEN + 4 + NLMSG_HDRLEN;
        struct test_nlmsghdr *err = (struct test_nlmsghdr *)b.buffer;
        err->nlmsg_len = (uint32_t)b.length;
        err->nlmsg_type = 2;
        err->nlmsg_flags = 0;
        err->nlmsg_seq = header->nlmsg_seq;
        int32_t code = m->deny_channel ? -EPERM : 0;
        memcpy(b.buffer + NLMSG_HDRLEN, &code, sizeof(code));
        memcpy(b.buffer + NLMSG_HDRLEN + 4, request, NLMSG_HDRLEN);
        memcpy(reply, b.buffer, b.length);
        *reply_len = b.length;
        return 0;
    }

    return -1;
}

static const char *test_interface(void)
{
    return "test-wifi0";
}

static unsigned mock_ifindex(const char *name)
{
    return strcmp(name, test_interface()) == 0 ? 7u : 0u;
}

static void setup(mock *m)
{
    memset(m, 0, sizeof(*m));
    m->iftype = NERO_NL80211_IFTYPE_STATION;
    m->wiphy = 3;
    m->supported_iftypes = (1u << NERO_NL80211_IFTYPE_STATION) |
                           (1u << NERO_NL80211_IFTYPE_AP) |
                           (1u << NERO_NL80211_IFTYPE_MONITOR);
    nero_nl80211_test_set_exchange(m, mock_exchange);
    nero_nl80211_test_set_ifindex(mock_ifindex);
}

static void test_invalid_interface(void)
{
    mock m;
    setup(&m);
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info("nero-no-such-iface-xyz", &info, error, sizeof(error)) != 0);
    CHECK(error[0] != '\0');
    CHECK(strstr(error, "does not exist") != NULL);
    CHECK(m.ctrl_requests == 0);
    CHECK(nero_nl80211_set_frequency("nero-no-such-iface-xyz", 2437, error,
                                     sizeof(error)) != 0);
    CHECK(strstr(error, "does not exist") != NULL);
    CHECK(m.ctrl_requests == 0);
}

static void test_info_parsing(void)
{
    mock m;
    setup(&m);
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) == 0);
    CHECK(info.available == 1);
    CHECK(info.ifindex == 7);
    CHECK(info.wiphy == 3);
    CHECK(info.has_iftype == 1);
    CHECK(info.iftype == NERO_NL80211_IFTYPE_STATION);
    CHECK(strcmp(info.mac, "02:00:00:00:00:01") == 0);
    CHECK(strcmp(info.phy_name, "phy0") == 0);
    CHECK(strcmp(info.regulatory, "US") == 0);
    CHECK(info.has_supported_iftypes == 1);
    CHECK(info.monitor_supported == 1);
    CHECK((info.supported_iftypes & (1u << NERO_NL80211_IFTYPE_AP)) != 0);
    CHECK(info.bands == (NERO_NL80211_BAND_2GHZ | NERO_NL80211_BAND_5GHZ |
                         NERO_NL80211_BAND_6GHZ));
    CHECK(info.channel_count == 4);
    CHECK(info.disabled_channel_count == 2);
    CHECK(info.no_ir_channel_count == 2);
    CHECK(m.get_requests == 1);
    CHECK(m.wiphy_requests == 1);
}

static void test_family_cache(void)
{
    mock m;
    setup(&m);
    nero_nl80211_info info;
    char error[256];
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) == 0);
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) == 0);
    CHECK(m.ctrl_requests == 1);
    CHECK(m.get_requests == 2);
    CHECK(m.wiphy_requests == 2);
}

static void test_set_success_and_restore(void)
{
    mock m;
    setup(&m);
    nero_nl80211_info verified;
    char error[256] = "";
    nero_nl80211_info before;
    CHECK(nero_nl80211_get_info(test_interface(), &before, error, sizeof(error)) == 0);
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_MONITOR, &verified, error,
                                sizeof(error)) == 0);
    CHECK(m.set_requests == 1);
    CHECK(m.last_set_ifindex == (uint32_t)before.ifindex);
    CHECK(m.last_set_iftype == NERO_NL80211_IFTYPE_MONITOR);
    CHECK(verified.iftype == NERO_NL80211_IFTYPE_MONITOR);
    CHECK(m.iftype == NERO_NL80211_IFTYPE_MONITOR);

    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_STATION, &verified, error,
                                sizeof(error)) == 0);
    CHECK(m.set_requests == 2);
    CHECK(verified.iftype == NERO_NL80211_IFTYPE_STATION);
    CHECK(m.iftype == NERO_NL80211_IFTYPE_STATION);
}

static void test_set_is_idempotent(void)
{
    mock m;
    setup(&m);
    nero_nl80211_info verified;
    char error[256] = "";
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_STATION, &verified, error,
                                sizeof(error)) == 0);
    CHECK(m.set_requests == 0);
    CHECK(verified.iftype == NERO_NL80211_IFTYPE_STATION);
}

static void test_verification_failure(void)
{
    mock m;
    setup(&m);
    m.refuse_verify = 1;
    char error[256] = "";
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_MONITOR, NULL, error,
                                sizeof(error)) != 0);
    CHECK(m.set_requests == 1);
    CHECK(strstr(error, "verification failed") != NULL);
    CHECK(m.iftype == NERO_NL80211_IFTYPE_STATION);
}

static void test_kernel_denial(void)
{
    mock m;
    setup(&m);
    m.deny_set = 1;
    char error[256] = "";
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_MONITOR, NULL, error,
                                sizeof(error)) != 0);
    CHECK(strstr(error, "Operation not permitted") != NULL);
    CHECK(m.iftype == NERO_NL80211_IFTYPE_STATION);
}

static void test_monitor_unsupported(void)
{
    mock m;
    setup(&m);
    m.supported_iftypes = (1u << NERO_NL80211_IFTYPE_STATION) |
                          (1u << NERO_NL80211_IFTYPE_AP);
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) == 0);
    CHECK(info.monitor_supported == 0);
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_MONITOR, NULL, error,
                                sizeof(error)) != 0);
    CHECK(strstr(error, "not supported") != NULL);
    CHECK(m.set_requests == 0);
}

static void test_unknown_support_refuses_monitor(void)
{
    mock m;
    setup(&m);
    m.omit_supported = 1;
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) == 0);
    CHECK(info.has_supported_iftypes == 0);
    CHECK(nero_nl80211_set_type(test_interface(), NERO_NL80211_IFTYPE_MONITOR, NULL, error,
                                sizeof(error)) != 0);
    CHECK(strstr(error, "unavailable") != NULL);
    CHECK(m.set_requests == 0);
}

static void test_malformed_reply(void)
{
    mock m;
    setup(&m);
    m.malformed = 1;
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) != 0);
    CHECK(error[0] != '\0');
}

static void test_family_failure(void)
{
    mock m;
    setup(&m);
    m.fail_family = 1;
    nero_nl80211_info info;
    char error[256] = "";
    CHECK(nero_nl80211_get_info(test_interface(), &info, error, sizeof(error)) != 0);
    CHECK(strstr(error, "not supported") != NULL || strstr(error, "nl80211") != NULL);
}

static void test_names_and_summary(void)
{
    CHECK(strcmp(nero_nl80211_iftype_name(NERO_NL80211_IFTYPE_STATION), "station/managed") == 0);
    CHECK(strcmp(nero_nl80211_iftype_name(NERO_NL80211_IFTYPE_MONITOR), "monitor") == 0);
    CHECK(strcmp(nero_nl80211_iftype_name(999), "unknown") == 0);
    char summary[64];
    CHECK(strcmp(nero_nl80211_band_summary(NERO_NL80211_BAND_2GHZ | NERO_NL80211_BAND_5GHZ,
                                           summary, sizeof(summary)),
                 "2.4 GHz, 5 GHz") == 0);
    CHECK(strcmp(nero_nl80211_band_summary(0, summary, sizeof(summary)), "unknown") == 0);
}

static void test_set_frequency_success(void)
{
    mock m;
    setup(&m);
    char error[256] = "";
    CHECK(nero_nl80211_set_frequency(test_interface(), 2437, error, sizeof(error)) == 0);
    CHECK(m.channel_requests == 1);
    CHECK(m.last_set_freq == 2437);
    CHECK(m.last_set_ifindex > 0);
}

static void test_set_frequency_rejects_out_of_range(void)
{
    mock m;
    setup(&m);
    char error[256] = "";
    CHECK(nero_nl80211_set_frequency(test_interface(), 100, error, sizeof(error)) != 0);
    CHECK(strstr(error, "range") != NULL);
    CHECK(m.channel_requests == 0);
}

static void test_set_frequency_denied(void)
{
    mock m;
    setup(&m);
    m.deny_channel = 1;
    char error[256] = "";
    CHECK(nero_nl80211_set_frequency(test_interface(), 5180, error, sizeof(error)) != 0);
    CHECK(strstr(error, "Operation not permitted") != NULL);
    CHECK(m.channel_requests == 1);
}

static void test_platform_availability(void)
{
#if !defined(__linux__)
    CHECK(nero_nl80211_available() == 0);
#endif
}

int main(void)
{
    test_invalid_interface();
    test_info_parsing();
    test_family_cache();
    test_set_success_and_restore();
    test_set_is_idempotent();
    test_verification_failure();
    test_kernel_denial();
    test_monitor_unsupported();
    test_unknown_support_refuses_monitor();
    test_malformed_reply();
    test_family_failure();
    test_names_and_summary();
    test_set_frequency_success();
    test_set_frequency_rejects_out_of_range();
    test_set_frequency_denied();
    test_platform_availability();
    nero_nl80211_test_reset();

    if (failures != 0) {
        fprintf(stderr, "%d nl80211 test(s) failed\n", failures);
        return 1;
    }
    printf("wifi-nl80211: all tests passed\n");
    return 0;
}
