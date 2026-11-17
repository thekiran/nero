/* Read-only libpcap capture backend.
 *
 * libpcap is loaded at run time, so NERO builds without libpcap development
 * headers. On Windows the same ABI is served by Npcap (wpcap.dll) when the
 * user has installed it. macOS RFMON first disassociates and pins the current
 * channel through public CoreWLAN APIs. No injection or private ioctls. */

#ifdef _WIN32
/* Included first so WIN32_LEAN_AND_MEAN is set before <windows.h>: that keeps
 * <ole2.h> out and stops its `interface` macro from colliding with the
 * `interface` fields/parameters the shared capture code uses. */
#include "windows/nero_win_compat.h"
#endif

#include "nero/wifi_capture.h"
#include "nero/wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__) && !defined(NERO_SAFE_TEST)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#if defined(_WIN32)
/* <windows.h> arrived through nero_win_compat.h above (with WIN32_LEAN_AND_MEAN). */
typedef HMODULE nero_library;
#define NERO_LIBRARY_INVALID NULL
static nero_library library_open(const char *name)
{
    return LoadLibraryA(name);
}
static void *library_symbol(nero_library library, const char *name)
{
    union {
        FARPROC function;
        void *object;
    } symbol;
    symbol.function = GetProcAddress(library, name);
    return symbol.object;
}
static void library_close(nero_library library)
{
    FreeLibrary(library);
}
#else
#include <dlfcn.h>
typedef void *nero_library;
#define NERO_LIBRARY_INVALID NULL
static nero_library library_open(const char *name)
{
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}
static void *library_symbol(nero_library library, const char *name)
{
    return dlsym(library, name);
}
static void library_close(nero_library library)
{
    dlclose(library);
}
#endif

typedef struct pcap pcap_t;

struct nero_pcap_timeval {
    long tv_sec;
    long tv_usec;
};

struct nero_pcap_pkthdr {
    struct nero_pcap_timeval ts;
    uint32_t caplen;
    uint32_t len;
};

struct nero_pcap_stat {
    unsigned int ps_recv;
    unsigned int ps_drop;
    unsigned int ps_ifdrop;
};

typedef pcap_t *(*pcap_create_fn)(const char *, char *);
typedef int (*pcap_set_snaplen_fn)(pcap_t *, int);
typedef int (*pcap_set_promisc_fn)(pcap_t *, int);
typedef int (*pcap_set_timeout_fn)(pcap_t *, int);
typedef int (*pcap_set_immediate_fn)(pcap_t *, int);
typedef int (*pcap_set_rfmon_fn)(pcap_t *, int);
typedef int (*pcap_can_set_rfmon_fn)(pcap_t *);
typedef int (*pcap_activate_fn)(pcap_t *);
typedef char *(*pcap_geterr_fn)(pcap_t *);
typedef int (*pcap_datalink_fn)(pcap_t *);
typedef int (*pcap_set_datalink_fn)(pcap_t *, int);
typedef int (*pcap_setnonblock_fn)(pcap_t *, int, char *);
typedef int (*pcap_next_ex_fn)(pcap_t *, struct nero_pcap_pkthdr **, const unsigned char **);
typedef int (*pcap_stats_fn)(pcap_t *, struct nero_pcap_stat *);
typedef void (*pcap_close_fn)(pcap_t *);

/* dlsym/GetProcAddress return object pointers. ISO C does not permit casting
 * those to function pointers, so convert through a union whose members have
 * matching representations on the supported ABIs. */
typedef union {
    void *object;
    pcap_create_fn create;
    pcap_set_snaplen_fn set_snaplen;
    pcap_set_promisc_fn set_promisc;
    pcap_set_timeout_fn set_timeout;
    pcap_set_immediate_fn set_immediate;
    pcap_set_rfmon_fn set_rfmon;
    pcap_can_set_rfmon_fn can_set_rfmon;
    pcap_activate_fn activate;
    pcap_geterr_fn geterr;
    pcap_datalink_fn datalink;
    pcap_set_datalink_fn set_datalink;
    pcap_setnonblock_fn setnonblock;
    pcap_next_ex_fn next_ex;
    pcap_stats_fn stats;
    pcap_close_fn close;
} nero_pcap_symbol;

typedef struct {
    nero_library library;
    pcap_create_fn create;
    pcap_set_snaplen_fn set_snaplen;
    pcap_set_promisc_fn set_promisc;
    pcap_set_timeout_fn set_timeout;
    pcap_set_immediate_fn set_immediate;
    pcap_set_rfmon_fn set_rfmon;
    pcap_can_set_rfmon_fn can_set_rfmon;
    pcap_activate_fn activate;
    pcap_geterr_fn geterr;
    pcap_datalink_fn datalink;
    pcap_set_datalink_fn set_datalink;
    pcap_setnonblock_fn setnonblock;
    pcap_next_ex_fn next_ex;
    pcap_stats_fn stats;
    pcap_close_fn close;
    char backend[64];
} nero_pcap_api;

struct nero_wifi_capture {
    nero_pcap_api api;
    pcap_t *handle;
    int linktype;
    int monitor_requested;
    int monitor_set;
    int link_fallback;
    char backend[64];
    char interface[NERO_WIFI_IFNAME_MAX_CAPTURE];
};

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static int prepare_monitor_radio(const char *name, char *error, size_t error_len)
{
#if defined(__APPLE__) && !defined(NERO_SAFE_TEST)
    /* RFMON/DLT alone can succeed while an associated Apple radio delivers
     * nothing. Keep its existing channel, but leave station mode first.
     * Use a local pool: capture may be opened from a plain C worker thread. */
    id (*object)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
    id pool = object((id)objc_getClass("NSAutoreleasePool"), sel_registerName("new"));
    id client = object((id)objc_getClass("CWWiFiClient"),
                       sel_registerName("sharedWiFiClient"));
    id interface_name = ((id (*)(id, SEL, const char *))objc_msgSend)(
        (id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), name);
    id radio = ((id (*)(id, SEL, id))objc_msgSend)(
        client, sel_registerName("interfaceWithName:"), interface_name);
    id channel = object(radio, sel_registerName("wlanChannel"));
    int result = -1;
    if (radio == NULL || channel == NULL) {
        set_error(error, error_len,
                  "CoreWLAN has no current Wi-Fi channel; connect to a Wi-Fi network first");
    } else {
        channel = object(channel, sel_registerName("retain"));
        ((void (*)(id, SEL))objc_msgSend)(radio, sel_registerName("disassociate"));
        id channel_error = NULL;
        signed char ok = ((signed char (*)(id, SEL, id, id *))objc_msgSend)(
            radio, sel_registerName("setWLANChannel:error:"), channel, &channel_error);
        if (!ok) {
            id description = object(channel_error, sel_registerName("localizedDescription"));
            const char *detail = ((const char *(*)(id, SEL))objc_msgSend)(
                description, sel_registerName("UTF8String"));
            set_error(error, error_len, detail ? detail : "CoreWLAN could not set the monitor channel");
        } else {
            result = 0;
        }
        ((void (*)(id, SEL))objc_msgSend)(channel, sel_registerName("release"));
    }
    ((void (*)(id, SEL))objc_msgSend)(pool, sel_registerName("drain"));
    return result;
#else
    (void)name;
    (void)error;
    (void)error_len;
    return 0;
#endif
}

static const char *const library_names[] = {
#if defined(_WIN32)
    "wpcap.dll", "Packet.dll",
#elif defined(__APPLE__)
    "/usr/lib/libpcap.A.dylib", "libpcap.A.dylib", "libpcap.dylib",
#else
    "libpcap.so.1", "libpcap.so.0.8", "libpcap.so",
#endif
    NULL};

static nero_library open_library(char *backend, size_t backend_len)
{
    for (size_t i = 0; library_names[i] != NULL; ++i) {
        nero_library library = library_open(library_names[i]);
        if (library != NERO_LIBRARY_INVALID) {
            if (backend != NULL && backend_len > 0) {
                snprintf(backend, backend_len, "%s", library_names[i]);
            }
            return library;
        }
    }
    return NERO_LIBRARY_INVALID;
}

static int load_api(nero_pcap_api *api, char *error, size_t error_len)
{
    memset(api, 0, sizeof(*api));
    api->library = open_library(api->backend, sizeof(api->backend));
    if (api->library == NERO_LIBRARY_INVALID) {
        set_error(error, error_len, "libpcap/Npcap could not be loaded");
        return -1;
    }
#define LOAD_REQUIRED(field, name)                                                                 \
    do {                                                                                           \
        nero_pcap_symbol symbol;                                                                   \
        symbol.object = library_symbol(api->library, name);                                        \
        if (symbol.object == NULL) {                                                               \
            set_error(error, error_len, "libpcap is missing " name);                               \
            library_close(api->library);                                                           \
            api->library = NERO_LIBRARY_INVALID;                                                   \
            return -1;                                                                             \
        }                                                                                          \
        api->field = symbol.field;                                                                 \
    } while (0)
    LOAD_REQUIRED(create, "pcap_create");
    LOAD_REQUIRED(set_snaplen, "pcap_set_snaplen");
    LOAD_REQUIRED(set_promisc, "pcap_set_promisc");
    LOAD_REQUIRED(set_timeout, "pcap_set_timeout");
    LOAD_REQUIRED(activate, "pcap_activate");
    LOAD_REQUIRED(geterr, "pcap_geterr");
    LOAD_REQUIRED(datalink, "pcap_datalink");
    LOAD_REQUIRED(set_datalink, "pcap_set_datalink");
    LOAD_REQUIRED(setnonblock, "pcap_setnonblock");
    LOAD_REQUIRED(next_ex, "pcap_next_ex");
    LOAD_REQUIRED(stats, "pcap_stats");
    LOAD_REQUIRED(close, "pcap_close");
#undef LOAD_REQUIRED
    nero_pcap_symbol immediate;
    immediate.object = library_symbol(api->library, "pcap_set_immediate_mode");
    api->set_immediate = immediate.set_immediate;
    nero_pcap_symbol rfmon;
    rfmon.object = library_symbol(api->library, "pcap_set_rfmon");
    api->set_rfmon = rfmon.set_rfmon;
    nero_pcap_symbol can_rfmon;
    can_rfmon.object = library_symbol(api->library, "pcap_can_set_rfmon");
    api->can_set_rfmon = can_rfmon.can_set_rfmon;
    return 0;
}

static void unload_api(nero_pcap_api *api)
{
    if (api->library != NERO_LIBRARY_INVALID) {
        library_close(api->library);
        api->library = NERO_LIBRARY_INVALID;
    }
}

int nero_wifi_capture_probe(const char *interface_name, int *backend_available,
                            int *can_request_monitor, char *backend_name, size_t backend_name_len,
                            char *error, size_t error_len)
{
    if (backend_available == NULL || can_request_monitor == NULL) {
        set_error(error, error_len, "invalid probe arguments");
        return -1;
    }
    *backend_available = 0;
    *can_request_monitor = 0;
    if (backend_name != NULL && backend_name_len > 0)
        backend_name[0] = '\0';

    nero_pcap_api api;
    if (load_api(&api, error, error_len) != 0)
        return -1;
    *backend_available = 1;
    if (backend_name != NULL && backend_name_len > 0) {
        snprintf(backend_name, backend_name_len, "%s", api.backend);
    }

    if (api.can_set_rfmon != NULL && api.create != NULL && interface_name != NULL) {
        char errbuf[256] = {0};
        pcap_t *handle = api.create(interface_name, errbuf);
        if (handle != NULL) {
            *can_request_monitor = api.can_set_rfmon(handle) > 0 ? 1 : 0;
            api.close(handle);
        } else if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "%s", errbuf);
        }
    }
    unload_api(&api);
    return 0;
}

static int capture_open_handle(nero_wifi_capture *capture, int request_monitor, char *error,
                               size_t error_len)
{
    char errbuf[256] = {0};
    capture->handle = capture->api.create(capture->interface, errbuf);
    if (capture->handle == NULL) {
        set_error(error, error_len, errbuf[0] != '\0' ? errbuf : "pcap_create failed");
        return -1;
    }
    capture->api.set_snaplen(capture->handle, 65535);
    capture->api.set_promisc(capture->handle, 1);
    capture->api.set_timeout(capture->handle, 200);
    if (capture->api.set_immediate != NULL)
        capture->api.set_immediate(capture->handle, 1);

    if (request_monitor &&
        (capture->api.set_rfmon == NULL ||
         capture->api.set_rfmon(capture->handle, 1) != 0)) {
        set_error(error, error_len, "monitor request rejected; use --link for Ethernet capture");
        return -1;
    }
    if (request_monitor &&
        prepare_monitor_radio(capture->interface, error, error_len) != 0)
        return -1;
    int result = capture->api.activate(capture->handle);
    if (result < 0) {
        set_error(error, error_len, capture->api.geterr(capture->handle));
        return -1;
    }
    capture->linktype = capture->api.datalink(capture->handle);
    if (request_monitor) {
        /* Darwin can advertise multiple DLTs. Prefer radio metadata explicitly,
         * then raw 802.11; never treat Ethernet as a successful monitor capture. */
        if (capture->linktype != NERO_WIFI_LINKTYPE_RADIOTAP &&
            capture->api.set_datalink(capture->handle, NERO_WIFI_LINKTYPE_RADIOTAP) != 0 &&
            capture->linktype != NERO_WIFI_LINKTYPE_IEEE802_11)
            (void)capture->api.set_datalink(capture->handle, NERO_WIFI_LINKTYPE_IEEE802_11);
        capture->linktype = capture->api.datalink(capture->handle);
        if (capture->linktype != NERO_WIFI_LINKTYPE_IEEE802_11 &&
            capture->linktype != NERO_WIFI_LINKTYPE_RADIOTAP) {
            set_error(error, error_len,
                      "driver did not provide 802.11/radiotap; use --link for Ethernet capture");
            return -1;
        }
        capture->monitor_set = 1;
    }
    if (capture->linktype < 0) {
        set_error(error, error_len, "capture link type is unavailable");
        return -1;
    }
    /* A packet-buffer timeout is not a portable idle wakeup guarantee. */
    if (capture->api.setnonblock(capture->handle, 1, errbuf) != 0) {
        set_error(error, error_len, errbuf);
        return -1;
    }
    return 0;
}

nero_wifi_capture *nero_wifi_capture_open(const char *interface_name, int request_monitor,
                                          char *error, size_t error_len)
{
    if (interface_name == NULL || interface_name[0] == '\0') {
        set_error(error, error_len, "no capture interface specified");
        return NULL;
    }
    nero_wifi_capture *capture = calloc(1, sizeof(*capture));
    if (capture == NULL) {
        set_error(error, error_len, "out of memory");
        return NULL;
    }
    if (load_api(&capture->api, error, error_len) != 0) {
        free(capture);
        return NULL;
    }
    snprintf(capture->backend, sizeof(capture->backend), "%s", capture->api.backend);
    snprintf(capture->interface, sizeof(capture->interface), "%s", interface_name);
    capture->monitor_requested = request_monitor;

    if (capture_open_handle(capture, request_monitor, error, error_len) != 0) {
        nero_wifi_capture_close(capture);
        return NULL;
    }
    if (error != NULL && error_len > 0)
        error[0] = '\0';
    return capture;
}

int nero_wifi_capture_run(nero_wifi_capture *capture, nero_wifi_capture_callback callback,
                          void *context, _Atomic int *stop, int poll_ms,
                          nero_wifi_capture_summary *summary, char *error, size_t error_len)
{
    if (poll_ms <= 0 || poll_ms > 200)
        poll_ms = 200;
    if (capture == NULL || callback == NULL) {
        set_error(error, error_len, "invalid capture arguments");
        return -1;
    }
    if (summary != NULL) {
        memset(summary, 0, sizeof(*summary));
        summary->backend_available = 1;
        summary->monitor_requested = capture->monitor_requested;
        summary->monitor_set = capture->monitor_set;
        summary->link_fallback = capture->link_fallback;
        summary->linktype = capture->linktype;
        snprintf(summary->backend, sizeof(summary->backend), "%s", capture->backend);
        snprintf(summary->interface, sizeof(summary->interface), "%s", capture->interface);
    }

    for (;;) {
        if (stop != NULL && atomic_load_explicit(stop, memory_order_relaxed))
            break;
        struct nero_pcap_pkthdr *header = NULL;
        const unsigned char *data = NULL;
        int result = capture->api.next_ex(capture->handle, &header, &data);
        if (result == 0) {
            /* Silence is not evidence of unsupported hardware. Keep RFMON open. */
#ifdef _WIN32
            Sleep((DWORD)poll_ms);
#else
            struct timespec delay = {0, (long)poll_ms * 1000000L};
            (void)nanosleep(&delay, NULL);
#endif
            continue;
        }
        if (result == -2)
            break;
        if (result < 0) {
            set_error(error, error_len, capture->api.geterr(capture->handle));
            return -1;
        }
        if (header == NULL || data == NULL)
            continue;
        nero_wifi_capture_packet packet;
        packet.data = data;
        packet.caplen = header->caplen;
        packet.len = header->len;
        packet.linktype = capture->linktype;
        packet.timestamp_us = (int64_t)header->ts.tv_sec * 1000000LL + header->ts.tv_usec;
        if (summary != NULL) {
            ++summary->packets;
            summary->bytes += header->caplen;
        }
        if (callback(context, &packet) != 0) {
            if (summary != NULL) {
                struct nero_pcap_stat pcap_stats;
                memset(&pcap_stats, 0, sizeof(pcap_stats));
                if (capture->api.stats(capture->handle, &pcap_stats) == 0) {
                    summary->received = pcap_stats.ps_recv;
                    summary->dropped =
                        (uint64_t)pcap_stats.ps_drop + (uint64_t)pcap_stats.ps_ifdrop;
                }
            }
            return -1;
        }
    }

    if (summary != NULL) {
        struct nero_pcap_stat pcap_stats;
        memset(&pcap_stats, 0, sizeof(pcap_stats));
        if (capture->api.stats(capture->handle, &pcap_stats) == 0) {
            summary->received = pcap_stats.ps_recv;
            summary->dropped = (uint64_t)pcap_stats.ps_drop + (uint64_t)pcap_stats.ps_ifdrop;
        }
    }
    return stop != NULL && atomic_load_explicit(stop, memory_order_relaxed) ? 1 : 0;
}

void nero_wifi_capture_mode(const nero_wifi_capture *capture, int *monitor_set,
                            int *link_fallback, int *linktype)
{
    if (capture == NULL)
        return;
    if (monitor_set != NULL)
        *monitor_set = capture->monitor_set;
    if (link_fallback != NULL)
        *link_fallback = capture->link_fallback;
    if (linktype != NULL)
        *linktype = capture->linktype;
}

void nero_wifi_capture_close(nero_wifi_capture *capture)
{
    if (capture == NULL)
        return;
    if (capture->handle != NULL) {
        capture->api.close(capture->handle);
        capture->handle = NULL;
    }
    unload_api(&capture->api);
    free(capture);
}
