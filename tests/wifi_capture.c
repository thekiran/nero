/* Deterministic libpcap state-machine tests; never opens a radio or BPF device. */
#include <assert.h>
#ifndef NERO_SAFE_TEST
#define NERO_SAFE_TEST
#endif
#include "../src/platform/wifi_capture.c"

static int dlt, allow_dlt, activate_result, rfmon_result, nonblock_result;
static int creates, closes, polls, idle_polls, stop_on_idle;
static atomic_int stop_test;
static pcap_t *fake_create(const char *name, char *error)
{
    (void)name; (void)error; ++creates;
    return (pcap_t *)&dlt;
}
static int fake_option(pcap_t *p, int value) { (void)p; (void)value; return 0; }
static int fake_rfmon(pcap_t *p, int value) { (void)p; (void)value; return rfmon_result; }
static int fake_activate(pcap_t *p) { (void)p; return activate_result; }
static int fake_datalink(pcap_t *p) { (void)p; return dlt; }
static int fake_set_datalink(pcap_t *p, int value)
{
    (void)p;
    if (value != allow_dlt) return -1;
    dlt = value;
    return 0;
}
static int fake_nonblock(pcap_t *p, int value, char *error)
{
    (void)p; assert(value == 1);
    snprintf(error, 256, "nonblock failure");
    return nonblock_result;
}
static char *fake_error(pcap_t *p) { (void)p; return "activation failure"; }
static void fake_close(pcap_t *p) { (void)p; ++closes; }
static int fake_stats(pcap_t *p, struct nero_pcap_stat *s) { (void)p; (void)s; return 0; }
static int fake_next(pcap_t *p, struct nero_pcap_pkthdr **h, const unsigned char **data)
{
    static struct nero_pcap_pkthdr header = {{1, 0}, 1, 1};
    static unsigned char byte;
    (void)p;
    if (++polls <= idle_polls) {
        if (stop_on_idle) atomic_store(&stop_test, 1);
        return 0;
    }
    *h = &header; *data = &byte;
    atomic_store(&stop_test, 1);
    return 1;
}
static int callback(void *context, const nero_wifi_capture_packet *packet)
{
    int *seen = context;
    assert(packet->linktype == NERO_WIFI_LINKTYPE_RADIOTAP);
    ++*seen;
    return 0;
}
static nero_wifi_capture fixture(void)
{
    nero_wifi_capture c = {0};
    c.api.create = fake_create;
    c.api.set_snaplen = fake_option; c.api.set_promisc = fake_option;
    c.api.set_timeout = fake_option; c.api.set_rfmon = fake_rfmon;
    c.api.activate = fake_activate; c.api.datalink = fake_datalink;
    c.api.set_datalink = fake_set_datalink; c.api.setnonblock = fake_nonblock;
    c.api.geterr = fake_error; c.api.close = fake_close;
    c.api.stats = fake_stats; c.api.next_ex = fake_next;
    dlt = 1; allow_dlt = 127; activate_result = rfmon_result = nonblock_result = 0;
    creates = closes = polls = stop_on_idle = 0;
    atomic_store(&stop_test, 0);
    return c;
}
int main(void)
{
    char error[256] = {0};
    nero_wifi_capture c = fixture();
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == 0);
    assert(c.monitor_set && c.linktype == 127 && !c.link_fallback);
    /* Stay silent longer than the former 3-second fallback, then deliver data. */
    idle_polls = 16;
    int seen = 0;
    nero_wifi_capture_summary summary;
    assert(nero_wifi_capture_run(&c, callback, &seen, &stop_test, 200,
                                 &summary, error, sizeof(error)) == 1);
    assert(seen == 1 && summary.packets == 1 && creates == 1 && closes == 0);
    assert(summary.monitor_set && !summary.link_fallback);
    c = fixture(); stop_on_idle = 1;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == 0);
    assert(nero_wifi_capture_run(&c, callback, &seen, &stop_test, 1,
                                 &summary, error, sizeof(error)) == 1);
    assert(summary.packets == 0 && polls == 1 && creates == 1);
    c = fixture(); allow_dlt = 105;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == 0 && c.linktype == 105);
    c = fixture(); allow_dlt = 0;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == -1 && !c.monitor_set);
    c = fixture(); activate_result = -1;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == -1 && creates == 1);
    c = fixture(); rfmon_result = -1;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == -1);
    c = fixture(); c.api.set_rfmon = NULL;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == -1);
    c = fixture(); nonblock_result = -1;
    assert(capture_open_handle(&c, 1, error, sizeof(error)) == -1);
    c = fixture();
    assert(capture_open_handle(&c, 0, error, sizeof(error)) == 0 && dlt == 1);
    puts("wifi capture state-machine tests passed");
    return 0;
}
