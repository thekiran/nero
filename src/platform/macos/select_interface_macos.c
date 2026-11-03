/* macOS interface adapter.
 * Enumerates local network interfaces using getifaddrs(), identifies their
 * types with SystemConfiguration, and returns the name selected by the user.
 * Selection does not configure the interface or generate network traffic.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nero/select_interface.h"

#define MAX_INTERFACES 512
#define MAX_NAME_LEN 64

/* Same colors and labels as the Linux adapter. */
enum {
    COLOR_BLUE = 34,    /* Ethernet */
    COLOR_GREEN = 32,   /* Wireless */
    COLOR_YELLOW = 33,  /* Loopback */
    COLOR_RED = 31      /* Other */
};

typedef enum {
    TYPE_UNKNOWN,
    TYPE_ETHERNET,
    TYPE_WIRELESS,
    TYPE_LOOPBACK
} InterfaceType;

typedef struct {
    char name[MAX_NAME_LEN];
    InterfaceType type;
} InterfaceEntry;

/* Static storage keeps the returned name valid until the next call. */
static InterfaceEntry interfaces[MAX_INTERFACES];

static int find_interface(const char *name, int count)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(interfaces[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* AF_LINK gives one link-layer record per device (unlike IP address records). */
static int enumerate_interfaces(void)
{
    struct ifaddrs *head = NULL;
    int count = 0;

    if (getifaddrs(&head) != 0) {
        perror("getifaddrs");
        return -1;
    }

    for (const struct ifaddrs *item = head; item != NULL; item = item->ifa_next) {
        if (item->ifa_name == NULL || item->ifa_addr == NULL ||
            item->ifa_addr->sa_family != AF_LINK) {
            continue;
        }

        size_t name_len = strlen(item->ifa_name);
        if (name_len == 0 || name_len >= MAX_NAME_LEN ||
            find_interface(item->ifa_name, count) >= 0) {
            continue;
        }
        if (count >= MAX_INTERFACES) {
            break;
        }

        InterfaceEntry *entry = &interfaces[count];
        memcpy(entry->name, item->ifa_name, name_len + 1);
        entry->type = (item->ifa_flags & IFF_LOOPBACK)
                          ? TYPE_LOOPBACK : TYPE_UNKNOWN;
        ++count;
    }

    freeifaddrs(head);
    return count;
}

/* macOS can use en0 for Wi-Fi or Ethernet; do not guess from its name. */
static void classify_interfaces(int count)
{
    CFArrayRef all = SCNetworkInterfaceCopyAll();
    if (all == NULL) {
        return; /* Keep unknown types when metadata is unavailable. */
    }

    for (CFIndex i = 0; i < CFArrayGetCount(all); ++i) {
        SCNetworkInterfaceRef iface =
            (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
        CFStringRef bsd_name = SCNetworkInterfaceGetBSDName(iface);
        CFStringRef kind = SCNetworkInterfaceGetInterfaceType(iface);
        if (bsd_name == NULL || kind == NULL) {
            continue;
        }

        char name[MAX_NAME_LEN];
        if (!CFStringGetCString(bsd_name, name, sizeof(name),
                                kCFStringEncodingUTF8)) {
            continue;
        }

        int index = find_interface(name, count);
        if (index < 0 || interfaces[index].type == TYPE_LOOPBACK) {
            continue;
        }

        if (CFEqual(kind, kSCNetworkInterfaceTypeIEEE80211)) {
            interfaces[index].type = TYPE_WIRELESS;
        } else if (CFEqual(kind, kSCNetworkInterfaceTypeEthernet)) {
            interfaces[index].type = TYPE_ETHERNET;
        }
    }
