/* Platform Wi-Fi discovery.
 *
 * macOS uses CoreWLAN through the Objective-C runtime, so the project remains a
 * pure C build. CoreWLAN scan is active but never joins a network. Linux and
 * Windows expose read-only interface metadata; passive frames arrive through
 * the libpcap/Npcap capture backend. Any unsupported operation returns an
 * explicit error instead of fabricated results. */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "nero/wifi_platform.h"
#include "nero/wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <iphlpapi.h>
#include <windows.h>
#include <winsock2.h>
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
#else
#include <net/if.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

#if defined(__APPLE__) && !defined(NERO_NETWORK_UNAVAILABLE) && !defined(NERO_SAFE_TEST)
#include <objc/message.h>
#include <objc/runtime.h>
#define NERO_WIFI_COREWLAN 1
#endif

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
    size_t written = 0;
    while (value[written] != '\0' && written + 1 < out_len) {
        unsigned char ch = (unsigned char)value[written];
        out[written] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        ++written;
    }
    out[written] = '\0';
}

#if defined(NERO_WIFI_COREWLAN)

typedef signed char nero_objc_bool;

static nero_wifi_band band_from_corewlan(long band)
{
    if (band == 1)
        return NERO_WIFI_BAND_2GHZ;
    if (band == 2)
        return NERO_WIFI_BAND_5GHZ;
    if (band == 3)
        return NERO_WIFI_BAND_6GHZ;
    return NERO_WIFI_BAND_UNKNOWN;
}

static id msg_obj(id object, const char *selector)
{
    return ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static id msg_obj_obj(id object, const char *selector, id argument)
{
    return ((id (*)(id, SEL, id))objc_msgSend)(object, sel_registerName(selector), argument);
}

static id msg_scan(id object, const char *selector, id argument, nero_objc_bool hidden, id *error)
{
    return ((id (*)(id, SEL, id, nero_objc_bool, id *))objc_msgSend)(
        object, sel_registerName(selector), argument, hidden, error);
}

static nero_objc_bool msg_bool(id object, const char *selector)
{
    return ((nero_objc_bool (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static nero_objc_bool msg_supports(id object, const char *selector, long value)
{
    return ((nero_objc_bool (*)(id, SEL, long))objc_msgSend)(object, sel_registerName(selector),
                                                             value);
}

static long msg_long(id object, const char *selector)
{
    return ((long (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static id msg_index(id object, const char *selector, unsigned long index)
{
    return ((id (*)(id, SEL, unsigned long))objc_msgSend)(object, sel_registerName(selector),
                                                          index);
}

static const char *ns_utf8(id string)
{
    if (string == NULL)
        return NULL;
    return ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String"));
}

static id corewlan_client(void)
{
    id client_class = (id)objc_getClass("CWWiFiClient");
    if (client_class == NULL)
        return NULL;
    return msg_obj(client_class, "sharedWiFiClient");
}

static id autorelease_pool(void)
{
    id pool_class = (id)objc_getClass("NSAutoreleasePool");
    if (pool_class == NULL)
        return NULL;
    return msg_obj(pool_class, "new");
}

static void drain_pool(id pool)
{
    if (pool != NULL)
        msg_obj(pool, "drain");
}

static void fill_interface(id interface, nero_wifi_interface_info *info)
{
    memset(info, 0, sizeof(*info));
    if (interface == NULL)
        return;
    copy_text(info->name, sizeof(info->name), ns_utf8(msg_obj(interface, "interfaceName")));
    copy_text(info->hardware_address, sizeof(info->hardware_address),
              ns_utf8(msg_obj(interface, "hardwareAddress")));
    info->powered = msg_bool(interface, "powerOn") ? 1 : 0;
    info->service_active = msg_bool(interface, "serviceActive") ? 1 : 0;
    info->rssi = (int)msg_long(interface, "rssiValue");
    info->has_rssi = info->rssi != 0;
    id channel = msg_obj(interface, "wlanChannel");
    if (channel != NULL) {
        info->has_channel = 1;
        info->channel = (int)msg_long(channel, "channelNumber");
        info->band = band_from_corewlan(msg_long(channel, "channelBand"));
    }
    info->mode = (int)msg_long(interface, "interfaceMode");
}

static nero_wifi_security network_security(id network)
{
    if (msg_supports(network, "supportsSecurity:", 11) ||
        msg_supports(network, "supportsSecurity:", 12)) {
        return NERO_WIFI_SEC_WPA3;
    }
    if (msg_supports(network, "supportsSecurity:", 13))
        return NERO_WIFI_SEC_WPA2_WPA3;
    if (msg_supports(network, "supportsSecurity:", 14) ||
        msg_supports(network, "supportsSecurity:", 15)) {
        return NERO_WIFI_SEC_OWE;
    }
    if (msg_supports(network, "supportsSecurity:", 4) ||
        msg_supports(network, "supportsSecurity:", 5) ||
        msg_supports(network, "supportsSecurity:", 9) ||
        msg_supports(network, "supportsSecurity:", 10)) {
        return NERO_WIFI_SEC_WPA2;
    }
    if (msg_supports(network, "supportsSecurity:", 2) ||
        msg_supports(network, "supportsSecurity:", 3) ||
        msg_supports(network, "supportsSecurity:", 7) ||
        msg_supports(network, "supportsSecurity:", 8)) {
        return NERO_WIFI_SEC_WPA;
    }
    if (msg_supports(network, "supportsSecurity:", 1) ||
        msg_supports(network, "supportsSecurity:", 6)) {
        return NERO_WIFI_SEC_WEP;
    }
    if (msg_supports(network, "supportsSecurity:", 0))
        return NERO_WIFI_SEC_OPEN;
    return NERO_WIFI_SEC_UNKNOWN;
}

static void fill_network(id network, nero_wifi_ap *ap, int64_t now)
{
    memset(ap, 0, sizeof(*ap));
    ap->first_seen_us = now;
    ap->last_seen_us = now;
    ap->security = network_security(network);

    const char *bssid = ns_utf8(msg_obj(network, "bssid"));
    if (bssid != NULL && nero_wifi_parse_mac(bssid, ap->bssid) == 0) {
        ap->has_bssid = 1;
    } else {
        ap->redacted = 1;
    }

    const char *ssid = ns_utf8(msg_obj(network, "ssid"));
    if (ssid != NULL && ssid[0] != '\0') {
        size_t length = strlen(ssid);
        if (length > NERO_WIFI_SSID_MAX)
            length = NERO_WIFI_SSID_MAX;
        size_t written = 0;
        for (size_t i = 0; i < length; ++i) {
            unsigned char ch = (unsigned char)ssid[i];
            ap->ssid[written++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        ap->ssid[written] = '\0';
        ap->ssid_len = (uint8_t)written;
        ap->hidden = 0;
    } else {
        ap->hidden = 1;
    }

    id channel = msg_obj(network, "wlanChannel");
    if (channel != NULL) {
        ap->has_channel = 1;
        ap->channel = (int)msg_long(channel, "channelNumber");
        ap->band = band_from_corewlan(msg_long(channel, "channelBand"));
        int frequency = nero_wifi_channel_to_frequency(ap->channel, ap->band);
        if (frequency > 0) {
            ap->has_frequency = 1;
            ap->frequency_mhz = frequency;
        }
    }
    ap->rssi = (int)msg_long(network, "rssiValue");
    ap->has_rssi = ap->rssi != 0;
}

static void fill_host_apple(nero_wifi_platform_status *status)
{
    size_t size = sizeof(status->model);
    sysctlbyname("hw.model", status->model, &size, NULL, 0);
    size = sizeof(status->chip);
    sysctlbyname("machdep.cpu.brand_string", status->chip, &size, NULL, 0);
    size = sizeof(status->os_version);
    if (sysctlbyname("kern.osproductversion", status->os_version, &size, NULL, 0) != 0) {
        copy_text(status->os_version, sizeof(status->os_version), "unknown");
    }
    copy_text(status->os_name, sizeof(status->os_name), "macOS");
    struct utsname machine;
    if (uname(&machine) == 0) {
        copy_text(status->architecture, sizeof(status->architecture), machine.machine);
    }
    copy_text(status->driver, sizeof(status->driver), "CoreWLAN / IO80211 (Apple)");
}

int nero_wifi_platform_get_status(nero_wifi_platform_status *status, char *error, size_t error_len)
{
    if (status == NULL) {
        set_error(error, error_len, "invalid status argument");
        return -1;
    }
    memset(status, 0, sizeof(*status));
    fill_host_apple(status);

    id pool = autorelease_pool();
    id client = corewlan_client();
    status->capabilities.corewlan_available = client != NULL;
    copy_text(status->capabilities.backend, sizeof(status->capabilities.backend), "CoreWLAN");
    if (client != NULL) {
        id interface = msg_obj(client, "interface");
        if (interface != NULL) {
            status->capabilities.active_scan_available = 1;
            nero_wifi_interface_info info;
            fill_interface(interface, &info);
            copy_text(status->interface, sizeof(status->interface), info.name);
            status->wifi_powered = info.powered;
            status->service_active = info.service_active;
            status->has_rssi = info.has_rssi;
            status->rssi = info.rssi;
            status->has_channel = info.has_channel;
            status->channel = info.channel;
            status->band = info.band;
        }
    }
    drain_pool(pool);
    return 0;
}

int nero_wifi_platform_list_interfaces(nero_wifi_interface_info *out, int capacity, int *count,
                                       char *error, size_t error_len)
{
    if (out == NULL || count == NULL || capacity <= 0) {
        set_error(error, error_len, "invalid interface buffer");
        return -1;
    }
    *count = 0;
    id pool = autorelease_pool();
    id client = corewlan_client();
    if (client == NULL) {
        drain_pool(pool);
        set_error(error, error_len, "CoreWLAN is unavailable");
        return -1;
    }
    id names = msg_obj(client, "interfaceNames");
    if (names == NULL) {
        drain_pool(pool);
        return 0;
    }
    unsigned long total = (unsigned long)msg_long(names, "count");
    if (total > (unsigned long)capacity)
        total = (unsigned long)capacity;
    for (unsigned long i = 0; i < total; ++i) {
        id name = msg_index(names, "objectAtIndex:", i);
        if (name == NULL)
            continue;
        id interface = msg_obj_obj(client, "interfaceWithName:", name);
        if (interface == NULL)
            continue;
        fill_interface(interface, &out[*count]);
        ++*count;
    }
    drain_pool(pool);
    return 0;
}

int nero_wifi_platform_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                            nero_wifi_platform_capabilities *capabilities, char *error,
                            size_t error_len)
{
    if (out == NULL || count == NULL || capacity <= 0) {
        set_error(error, error_len, "invalid scan buffer");
        return -1;
    }
    *count = 0;
    if (capabilities != NULL) {
        memset(capabilities, 0, sizeof(*capabilities));
        capabilities->corewlan_available = 1;
        capabilities->active_scan_available = 1;
        copy_text(capabilities->backend, sizeof(capabilities->backend), "CoreWLAN (active scan)");
    }

    id pool = autorelease_pool();
    id client = corewlan_client();
    if (client == NULL) {
        drain_pool(pool);
        set_error(error, error_len, "CoreWLAN is unavailable");
        return -1;
    }
    id interface = NULL;
    if (interface_name != NULL && interface_name[0] != '\0') {
        id name = ((id (*)(id, SEL, const char *))objc_msgSend)(
            (id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
            interface_name);
        if (name != NULL)
            interface = msg_obj_obj(client, "interfaceWithName:", name);
    }
    if (interface == NULL)
        interface = msg_obj(client, "interface");
    if (interface == NULL) {
        drain_pool(pool);
        set_error(error, error_len, "no Wi-Fi interface");
        return -1;
    }

    id scan_error = NULL;
    id networks =
        msg_scan(interface, "scanForNetworksWithName:includeHidden:error:", NULL, 1, &scan_error);
    if (networks == NULL) {
        const char *description = scan_error != NULL
                                      ? ns_utf8(msg_obj(scan_error, "localizedDescription"))
                                      : "CoreWLAN scan failed";
        set_error(error, error_len, description != NULL ? description : "CoreWLAN scan failed");
        drain_pool(pool);
        return -1;
    }

    id array = msg_obj(networks, "allObjects");
    if (array == NULL) {
        drain_pool(pool);
        return 0;
    }
    unsigned long total = (unsigned long)msg_long(array, "count");
    int redacted = 0;
    int64_t now = nero_wifi_now_us();
    for (unsigned long i = 0; i < total && *count < capacity; ++i) {
        id network = msg_index(array, "objectAtIndex:", i);
        if (network == NULL)
            continue;
        fill_network(network, &out[*count], now);
        if (!out[*count].has_bssid || out[*count].redacted)
            redacted = 1;
        ++*count;
    }
    if (capabilities != NULL)
        capabilities->identifiers_redacted = redacted;
    drain_pool(pool);
    return 0;
}

#elif defined(_WIN32)

static void fill_host_windows(nero_wifi_platform_status *status)
{
    copy_text(status->os_name, sizeof(status->os_name), "Windows");
    copy_text(status->os_version, sizeof(status->os_version), "unknown");
    copy_text(status->architecture, sizeof(status->architecture), "unknown");
    copy_text(status->model, sizeof(status->model), "unknown");
    copy_text(status->chip, sizeof(status->chip), "unknown");
    copy_text(status->driver, sizeof(status->driver), "Npcap (if installed)");
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        copy_text(status->architecture, sizeof(status->architecture), "x86_64");
    } else if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        copy_text(status->architecture, sizeof(status->architecture), "x86");
    } else if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        copy_text(status->architecture, sizeof(status->architecture), "arm64");
    }
}

static int windows_wifi_adapter(IP_ADAPTER_ADDRESSES **out, char *interface_name,
                                size_t interface_name_len, int *powered)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *addresses = NULL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        addresses = malloc(size);
        if (addresses == NULL)
            return -1;
        ULONG result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, addresses, &size);
        if (result == NO_ERROR)
            break;
        free(addresses);
        addresses = NULL;
        if (result != ERROR_BUFFER_OVERFLOW)
            return -1;
    }
    if (addresses == NULL)
        return -1;
    for (IP_ADAPTER_ADDRESSES *entry = addresses; entry != NULL; entry = entry->Next) {
        if (entry->IfType != IF_TYPE_IEEE80211)
            continue;
        if (interface_name != NULL && interface_name_len > 0) {
            snprintf(interface_name, interface_name_len, "\\Device\\NPF_%s", entry->AdapterName);
        }
        if (powered != NULL)
            *powered = entry->OperStatus == IfOperStatusUp ? 1 : 0;
        *out = addresses;
        return 0;
    }
    free(addresses);
    return -1;
}

int nero_wifi_platform_get_status(nero_wifi_platform_status *status, char *error, size_t error_len)
{
    if (status == NULL) {
        set_error(error, error_len, "invalid status argument");
        return -1;
    }
    memset(status, 0, sizeof(*status));
    fill_host_windows(status);
    copy_text(status->capabilities.backend, sizeof(status->capabilities.backend), "Npcap");
    IP_ADAPTER_ADDRESSES *addresses = NULL;
    int powered = 0;
    if (windows_wifi_adapter(&addresses, status->interface, sizeof(status->interface), &powered) ==
        0) {
        status->capabilities.active_scan_available = 0;
        status->wifi_powered = powered;
        status->service_active = powered;
        free(addresses);
    }
    (void)error;
    (void)error_len;
    return 0;
}

int nero_wifi_platform_list_interfaces(nero_wifi_interface_info *out, int capacity, int *count,
                                       char *error, size_t error_len)
{
    if (out == NULL || count == NULL || capacity <= 0) {
        set_error(error, error_len, "invalid interface buffer");
        return -1;
    }
    *count = 0;
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *addresses = NULL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        addresses = malloc(size);
        if (addresses == NULL)
            return -1;
        ULONG result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, addresses, &size);
        if (result == NO_ERROR)
            break;
        free(addresses);
        addresses = NULL;
        if (result != ERROR_BUFFER_OVERFLOW) {
            set_error(error, error_len, "GetAdaptersAddresses failed");
            return -1;
        }
    }
    if (addresses == NULL)
        return -1;
    for (IP_ADAPTER_ADDRESSES *entry = addresses; entry != NULL && *count < capacity;
         entry = entry->Next) {
        if (entry->IfType != IF_TYPE_IEEE80211)
            continue;
        nero_wifi_interface_info *info = &out[*count];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "\\Device\\NPF_%s", entry->AdapterName);
        if (entry->PhysicalAddressLength == NERO_WIFI_MAC_LEN) {
            nero_wifi_format_mac(entry->PhysicalAddress, info->hardware_address,
                                 sizeof(info->hardware_address));
        }
        info->powered = entry->OperStatus == IfOperStatusUp ? 1 : 0;
        info->service_active = info->powered;
        ++*count;
    }
    free(addresses);
    return 0;
}

int nero_wifi_platform_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                            nero_wifi_platform_capabilities *capabilities, char *error,
                            size_t error_len)
{
    (void)interface_name;
    (void)out;
    (void)capacity;
    (void)count;
    (void)capabilities;
    set_error(error, error_len,
              "Windows active Wi-Fi scan is not implemented; use Npcap monitor-mode capture "
              "or a pcap/pcapng file");
    return -1;
}

#else

static void fill_host_posix(nero_wifi_platform_status *status)
{
    struct utsname info;
    if (uname(&info) == 0) {
        copy_text(status->os_name, sizeof(status->os_name), info.sysname);
        copy_text(status->os_version, sizeof(status->os_version), info.release);
        copy_text(status->architecture, sizeof(status->architecture), info.machine);
    }
#if defined(__APPLE__)
    size_t size = sizeof(status->model);
    sysctlbyname("hw.model", status->model, &size, NULL, 0);
    size = sizeof(status->chip);
    sysctlbyname("machdep.cpu.brand_string", status->chip, &size, NULL, 0);
    copy_text(status->os_name, sizeof(status->os_name), "macOS");
    size = sizeof(status->os_version);
    if (sysctlbyname("kern.osproductversion", status->os_version, &size, NULL, 0) != 0) {
        copy_text(status->os_version, sizeof(status->os_version), "unknown");
    }
    copy_text(status->driver, sizeof(status->driver), "CoreWLAN unavailable in this build");
#else
    copy_text(status->os_name, sizeof(status->os_name), "Linux");
    FILE *model = fopen("/sys/devices/virtual/dmi/id/product_name", "r");
    if (model != NULL) {
        if (fgets(status->model, sizeof(status->model), model) != NULL) {
            size_t length = strlen(status->model);
            if (length > 0 && status->model[length - 1] == '\n')
                status->model[length - 1] = '\0';
        }
        fclose(model);
    }
    FILE *cpu = fopen("/proc/cpuinfo", "r");
    if (cpu != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), cpu) != NULL) {
            if (strncmp(line, "model name", 10) == 0) {
                char *value = strchr(line, ':');
                if (value != NULL) {
                    ++value;
                    while (*value == ' ' || *value == '\t')
                        ++value;
                    size_t length = strlen(value);
                    if (length > 0 && value[length - 1] == '\n')
                        value[length - 1] = '\0';
                    copy_text(status->chip, sizeof(status->chip), value);
                }
                break;
            }
        }
        fclose(cpu);
    }
    copy_text(status->driver, sizeof(status->driver), "Linux sysfs; monitor-mode pcap capture");
#endif
}

static int posix_is_wireless(const char *name)
{
    if (name == NULL)
        return 0;
#if defined(__linux__)
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    if (access(path, F_OK) == 0)
        return 1;
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", name);
    return access(path, F_OK) == 0;
#else
    (void)name;
    return 0;
#endif
}

static void posix_find_interface(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
        return;
    out[0] = '\0';
    struct if_nameindex *interfaces = if_nameindex();
    if (interfaces == NULL)
        return;
    for (struct if_nameindex *entry = interfaces; entry->if_index != 0 && entry->if_name != NULL;
         ++entry) {
        if (posix_is_wireless(entry->if_name)) {
            copy_text(out, out_len, entry->if_name);
            break;
        }
    }
    if_freenameindex(interfaces);
}

int nero_wifi_platform_get_status(nero_wifi_platform_status *status, char *error, size_t error_len)
{
    if (status == NULL) {
        set_error(error, error_len, "invalid status argument");
        return -1;
    }
    memset(status, 0, sizeof(*status));
    fill_host_posix(status);
    posix_find_interface(status->interface, sizeof(status->interface));
    copy_text(status->capabilities.backend, sizeof(status->capabilities.backend),
              "passive pcap; platform scan unavailable");
    return 0;
}

int nero_wifi_platform_list_interfaces(nero_wifi_interface_info *out, int capacity, int *count,
                                       char *error, size_t error_len)
{
    if (out == NULL || count == NULL || capacity <= 0) {
        set_error(error, error_len, "invalid interface buffer");
        return -1;
    }
    *count = 0;
    struct if_nameindex *interfaces = if_nameindex();
    if (interfaces == NULL) {
        set_error(error, error_len, "interface enumeration failed");
        return -1;
    }
    for (struct if_nameindex *entry = interfaces;
         entry->if_index != 0 && entry->if_name != NULL && *count < capacity; ++entry) {
        if (!posix_is_wireless(entry->if_name))
            continue;
        nero_wifi_interface_info *info = &out[*count];
        memset(info, 0, sizeof(*info));
        copy_text(info->name, sizeof(info->name), entry->if_name);
#if defined(__linux__)
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", entry->if_name);
        FILE *address = fopen(path, "r");
        if (address != NULL) {
            char line[64];
            if (fgets(line, sizeof(line), address) != NULL) {
                size_t length = strlen(line);
                if (length > 0 && line[length - 1] == '\n')
                    line[length - 1] = '\0';
                uint8_t mac[NERO_WIFI_MAC_LEN];
                if (nero_wifi_parse_mac(line, mac) == 0) {
                    nero_wifi_format_mac(mac, info->hardware_address,
                                         sizeof(info->hardware_address));
                }
            }
            fclose(address);
        }
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", entry->if_name);
        FILE *state = fopen(path, "r");
        if (state != NULL) {
            char line[32];
            if (fgets(line, sizeof(line), state) != NULL) {
                info->powered = strncmp(line, "up", 2) == 0;
                info->service_active = info->powered;
            }
            fclose(state);
        }
#endif
        ++*count;
    }
    if_freenameindex(interfaces);
    return 0;
}

int nero_wifi_platform_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                            nero_wifi_platform_capabilities *capabilities, char *error,
                            size_t error_len)
{
    (void)interface_name;
    (void)out;
    (void)capacity;
    (void)count;
    if (capabilities != NULL) {
        memset(capabilities, 0, sizeof(*capabilities));
        copy_text(capabilities->backend, sizeof(capabilities->backend),
                  "unsupported on this platform");
    }
    set_error(error, error_len,
              "active Wi-Fi scanning is unavailable on this platform; import a monitor-mode "
              "pcap or use passive capture");
    return -1;
}

#endif
