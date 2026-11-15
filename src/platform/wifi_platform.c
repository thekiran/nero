/* Platform Wi-Fi discovery.
 *
 * macOS reads interface metadata through the Objective-C runtime. Active scans
 * run inside the signed NEROWiFi app with its own Location authorization. Linux and
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
/* The compat header sets WIN32_LEAN_AND_MEAN and _WIN32_WINNT, and includes
 * <winsock2.h>/<ws2tcpip.h>/<windows.h> in the correct order; <iphlpapi.h>
 * (GetAdaptersAddresses, IP_ADAPTER_ADDRESSES) must follow winsock2. */
#include "windows/nero_win_compat.h"
#include <iphlpapi.h>
/* Windows COM headers define interface as struct. Keep the Wi-Fi status
 * member name usable after all Windows headers have been included. */
#ifdef interface
#undef interface
#endif
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/socket.h>
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

static nero_objc_bool msg_bool(id object, const char *selector)
{
    return ((nero_objc_bool (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
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

#include "macos/wifi_helper_client.h"

int nero_wifi_platform_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                            nero_wifi_platform_capabilities *capabilities, char *error,
                            size_t error_len)
{
    if (!out || !count || capacity <= 0) {
        set_error(error, error_len, "invalid scan buffer");
        return -1;
    }
    return helper_scan(interface_name, out, capacity, count, capabilities, error, error_len);
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

/* ---- Read-only OS neighbor cache --------------------------------------- *
 * Provides a strictly local fallback for devices that cannot deliver 802.11
 * monitor frames. The ARP table is passive state; reading it sends no probes,
 * joins no network, and changes no interface. An entry is a device that has
 * recently answered on the local link, not a proven Wi-Fi station. */
#if defined(NERO_SAFE_TEST) || defined(NERO_NETWORK_UNAVAILABLE)
int nero_wifi_platform_neighbor_cache(const char *interface_name, nero_wifi_neighbor *out,
                                      int capacity, char *error, size_t error_len)
{
    (void)interface_name;
    (void)out;
    (void)capacity;
    set_error(error, error_len, "the OS neighbor cache is unavailable in this build");
    return 0;
}
#else
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
static int neighbor_mac_valid(const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    static const uint8_t zero[NERO_WIFI_MAC_LEN] = {0};
    if (memcmp(mac, zero, NERO_WIFI_MAC_LEN) == 0)
        return 0;
    return (mac[0] & 0x01) == 0; /* unicast only: skip multicast/broadcast */
}
#endif

#if defined(_WIN32)
int nero_wifi_platform_neighbor_cache(const char *interface_name, nero_wifi_neighbor *out,
                                      int capacity, char *error, size_t error_len)
{
    (void)interface_name;
    if (out == NULL || capacity <= 0)
        return 0;
    ULONG size = 0;
    DWORD status = GetIpNetTable(NULL, &size, FALSE);
    if (status != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        set_error(error, error_len, "the OS IPv4 neighbor table is unavailable");
        return 0;
    }
    PMIB_IPNETTABLE table = (PMIB_IPNETTABLE)malloc(size);
    if (table == NULL) {
        set_error(error, error_len, "out of memory");
        return 0;
    }
    status = GetIpNetTable(table, &size, FALSE);
    if (status != NO_ERROR) {
        free(table);
        set_error(error, error_len, "could not read the OS IPv4 neighbor table");
        return 0;
    }
    int count = 0;
    for (DWORD index = 0; index < table->dwNumEntries && count < capacity; ++index) {
        const MIB_IPNETROW *row = &table->table[index];
        if (row->dwPhysAddrLen != NERO_WIFI_MAC_LEN)
            continue;
        uint8_t mac[NERO_WIFI_MAC_LEN];
        memcpy(mac, row->bPhysAddr, NERO_WIFI_MAC_LEN);
        if (!neighbor_mac_valid(mac))
            continue;
        nero_wifi_neighbor *neighbor = &out[count++];
        memset(neighbor, 0, sizeof(*neighbor));
        memcpy(neighbor->mac, mac, NERO_WIFI_MAC_LEN);
        struct in_addr address;
        address.s_addr = row->dwAddr;
        if (inet_ntop(AF_INET, &address, neighbor->ip, sizeof(neighbor->ip)) == NULL)
            neighbor->ip[0] = '\0';
    }
    free(table);
    if (error != NULL && error_len > 0)
        error[0] = '\0';
    return count;
}
#elif defined(__linux__)
int nero_wifi_platform_neighbor_cache(const char *interface_name, nero_wifi_neighbor *out,
                                      int capacity, char *error, size_t error_len)
{
    if (out == NULL || capacity <= 0)
        return 0;
    FILE *file = fopen("/proc/net/arp", "r");
    if (file == NULL) {
        set_error(error, error_len, "could not read /proc/net/arp");
        return 0;
    }
    char line[256];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }
    int count = 0;
    while (count < capacity && fgets(line, sizeof(line), file) != NULL) {
        char ip[64], hardware[64], mask[64], device[64];
        unsigned type = 0, flags = 0;
        if (sscanf(line, "%63s 0x%x 0x%x %63s %63s %63s", ip, &type, &flags, hardware, mask,
                   device) != 6)
            continue;
        (void)type;
        if ((flags & 0x2) == 0) /* incomplete entry: no verified MAC */
            continue;
        if (interface_name != NULL && interface_name[0] != '\0' &&
            strcmp(interface_name, device) != 0)
            continue;
        unsigned int bytes[NERO_WIFI_MAC_LEN];
        if (sscanf(hardware, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                   &bytes[4], &bytes[5]) != NERO_WIFI_MAC_LEN)
            continue;
        uint8_t mac[NERO_WIFI_MAC_LEN];
        for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i)
            mac[i] = (uint8_t)bytes[i];
        if (!neighbor_mac_valid(mac))
            continue;
        nero_wifi_neighbor *neighbor = &out[count++];
        memset(neighbor, 0, sizeof(*neighbor));
        memcpy(neighbor->mac, mac, NERO_WIFI_MAC_LEN);
        snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
    }
    fclose(file);
    if (error != NULL && error_len > 0)
        error[0] = '\0';
    return count;
}
#elif defined(__APPLE__)
#define NERO_ROUNDUP(value) ((value) > 0 ? (1 + (((value) - 1) | 3)) : 4)

static int neighbor_from_mac_text(const char *mac_text, uint8_t mac[NERO_WIFI_MAC_LEN])
{
    unsigned int bytes[NERO_WIFI_MAC_LEN];
    if (sscanf(mac_text, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
               &bytes[4], &bytes[5]) != NERO_WIFI_MAC_LEN)
        return 0;
    for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i)
        mac[i] = (uint8_t)bytes[i];
    return neighbor_mac_valid(mac);
}

int nero_wifi_platform_neighbor_cache(const char *interface_name, nero_wifi_neighbor *out,
                                      int capacity, char *error, size_t error_len)
{
    if (out == NULL || capacity <= 0)
        return 0;
    int count = 0;
    /* Modern macOS returns the IPv4 ARP table through this sysctl only on some
     * releases, so try it first and fall back to the system arp tool. */
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_LLINFO};
    size_t needed = 0;
    if (sysctl(mib, 6, NULL, &needed, NULL, 0) == 0 && needed > 0) {
        char *buffer = malloc(needed);
        if (buffer != NULL) {
            size_t actual = needed;
            if (sysctl(mib, 6, buffer, &actual, NULL, 0) == 0) {
                char *cursor = buffer;
                char *limit = buffer + actual;
                while (cursor + sizeof(struct rt_msghdr) <= limit) {
                    struct rt_msghdr *message = (struct rt_msghdr *)cursor;
                    if (message->rtm_msglen == 0 || cursor + message->rtm_msglen > limit)
                        break;
                    if ((message->rtm_flags & RTF_LLINFO) != 0 && message->rtm_addrs != 0) {
                        struct sockaddr *address = (struct sockaddr *)(message + 1);
                        struct sockaddr_in *destination = NULL;
                        struct sockaddr_dl *link = NULL;
                        char name[IFNAMSIZ] = {0};
                        for (int index = 0; index < RTAX_MAX; ++index) {
                            if ((message->rtm_addrs & (1 << index)) == 0)
                                continue;
                            if (address->sa_len == 0)
                                break;
                            if (index == RTAX_DST && address->sa_family == AF_INET)
                                destination = (struct sockaddr_in *)address;
                            else if (index == RTAX_GATEWAY && address->sa_family == AF_LINK)
                                link = (struct sockaddr_dl *)address;
                            else if (index == RTAX_IFP && address->sa_family == AF_LINK) {
                                struct sockaddr_dl *ifp = (struct sockaddr_dl *)address;
                                if (ifp->sdl_nlen > 0 && ifp->sdl_nlen < IFNAMSIZ)
                                    memcpy(name, ifp->sdl_data, ifp->sdl_nlen);
                            }
                            address = (struct sockaddr *)((char *)address +
                                                          NERO_ROUNDUP(address->sa_len));
                        }
                        if (destination != NULL && link != NULL &&
                            link->sdl_alen == NERO_WIFI_MAC_LEN) {
                            const uint8_t *mac = (const uint8_t *)LLADDR(link);
                            int matches = interface_name == NULL || interface_name[0] == '\0';
                            if (!matches && name[0] != '\0')
                                matches = strcmp(interface_name, name) == 0;
                            if (!matches && name[0] == '\0' && interface_name != NULL)
                                matches = link->sdl_index == if_nametoindex(interface_name);
                            if (neighbor_mac_valid(mac) && matches && count < capacity) {
                                nero_wifi_neighbor *neighbor = &out[count++];
                                memset(neighbor, 0, sizeof(*neighbor));
                                memcpy(neighbor->mac, mac, NERO_WIFI_MAC_LEN);
                                if (inet_ntop(AF_INET, &destination->sin_addr, neighbor->ip,
                                              sizeof(neighbor->ip)) == NULL)
                                    neighbor->ip[0] = '\0';
                            }
                        }
                    }
                    cursor += message->rtm_msglen;
                }
            }
            free(buffer);
        }
    }
    if (count == 0) {
        FILE *pipe = popen("/usr/sbin/arp -an 2>/dev/null", "r");
        if (pipe != NULL) {
            char line[256];
            while (count < capacity && fgets(line, sizeof(line), pipe) != NULL) {
                char ip[64], mac_text[64], device[64];
                if (sscanf(line, "%*[^(](%63[^)]) at %63s on %63s", ip, mac_text, device) != 3)
                    continue;
                if (interface_name != NULL && interface_name[0] != '\0' &&
                    strcmp(interface_name, device) != 0)
                    continue;
                uint8_t mac[NERO_WIFI_MAC_LEN];
                if (!neighbor_from_mac_text(mac_text, mac))
                    continue;
                nero_wifi_neighbor *neighbor = &out[count++];
                memset(neighbor, 0, sizeof(*neighbor));
                memcpy(neighbor->mac, mac, NERO_WIFI_MAC_LEN);
                snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
            }
            pclose(pipe);
        }
    }
    /* Recent macOS hands the sandboxed CLI process an empty routing table, so
     * the in-process reads above return nothing even when the ARP cache has
     * entries. Fall back to the signed NEROWiFi.app helper, which LaunchServices
     * runs as its own responsible process and can read the real table. Needs no
     * Location permission; never probes or sends traffic. */
    if (count == 0 && getuid() == geteuid() && geteuid() != 0) {
        char helper_error[160] = {0};
        int helper_count = 0;
        if (helper_neighbors(interface_name, out, capacity, &helper_count, helper_error,
                             sizeof(helper_error)) == 0 &&
            helper_count > 0)
            count = helper_count;
    }
    if (count == 0)
        set_error(error, error_len, "could not read the OS neighbor cache");
    else if (error != NULL && error_len > 0)
        error[0] = '\0';
    return count;
}
#else
int nero_wifi_platform_neighbor_cache(const char *interface_name, nero_wifi_neighbor *out,
                                      int capacity, char *error, size_t error_len)
{
    (void)interface_name;
    (void)out;
    (void)capacity;
    set_error(error, error_len, "the OS neighbor cache is unavailable on this platform");
    return 0;
}
#endif
#endif
