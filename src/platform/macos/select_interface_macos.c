
/* macOS interface adapter. getifaddrs supplies local interface records and
 * SystemConfiguration supplies descriptive metadata for the selection list.
 * Platform-owned enumeration data must be released after building the list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include "nero/select_interface.h"

#define MAX_INTERFACES 128
#define MAX_NAME_LEN 64

enum {
    COLOR_RESET = 0,
    COLOR_BLUE = 34,
    COLOR_GREEN = 32,
    COLOR_YELLOW = 33,
    COLOR_RED = 31,
    COLOR_CYAN = 36
};

typedef enum {
    IFACE_ETHERNET,
    IFACE_WIRELESS,
    IFACE_LOOPBACK,
    IFACE_VIRTUAL,
    IFACE_OTHER
} InterfaceType;

typedef struct {
    char name[MAX_NAME_LEN];
    InterfaceType type;
    unsigned int flags;
} NetworkInterface;

static NetworkInterface interfaces[MAX_INTERFACES];

/* Identify macOS network interface type */
static InterfaceType get_interface_type(
    const char *name,
    unsigned int flags,
    CFArrayRef sc_interfaces
) {
    if (flags & IFF_LOOPBACK)
        return IFACE_LOOPBACK;

    /* Common macOS virtual interfaces */
    if (strncmp(name, "utun", 4) == 0 ||
        strncmp(name, "awdl", 4) == 0 ||
        strncmp(name, "llw", 3) == 0 ||
        strncmp(name, "bridge", 6) == 0 ||
        strncmp(name, "vmenet", 6) == 0 ||
        strncmp(name, "vmnet", 5) == 0 ||
        strncmp(name, "gif", 3) == 0 ||
        strncmp(name, "stf", 3) == 0) {
        return IFACE_VIRTUAL;
    }


    if (sc_interfaces) {
        CFIndex count = CFArrayGetCount(sc_interfaces);

        for (CFIndex i = 0; i < count; i++) {
            SCNetworkInterfaceRef iface =
                (SCNetworkInterfaceRef)
                CFArrayGetValueAtIndex(sc_interfaces, i);

            CFStringRef bsd_name =
                SCNetworkInterfaceGetBSDName(iface);

            if (!bsd_name)
                continue;

            char buffer[MAX_NAME_LEN];

            if (!CFStringGetCString(
                    bsd_name,
                    buffer,
                    sizeof(buffer),
                    kCFStringEncodingUTF8))
                continue;

            if (strcmp(buffer, name) != 0)
                continue;

            CFStringRef type =
                SCNetworkInterfaceGetInterfaceType(iface);

            if (!type)
                break;

            if (CFEqual(type,
                        kSCNetworkInterfaceTypeIEEE80211))
                return IFACE_WIRELESS;

            if (CFEqual(type,
                        kSCNetworkInterfaceTypeEthernet))
                return IFACE_ETHERNET;

            break;
        }
    }


    return IFACE_OTHER;
}

static const char *type_str(InterfaceType type) {
    switch (type) {
        case IFACE_ETHERNET: return "Ethernet";
        case IFACE_WIRELESS: return "Wireless";
        case IFACE_LOOPBACK: return "Loopback";
        case IFACE_VIRTUAL:  return "Virtual";
        default:             return "Unknown";
    }
}

static int type_color(InterfaceType type) {
    switch (type) {
        case IFACE_ETHERNET: return COLOR_BLUE;
        case IFACE_WIRELESS: return COLOR_GREEN;
        case IFACE_LOOPBACK: return COLOR_YELLOW;
        case IFACE_VIRTUAL:  return COLOR_CYAN;
        default:             return COLOR_RED;
    }
}

static void print_colored(
    int index,
    const NetworkInterface *iface
) {
    int color = type_color(iface->type);
    const char *term = getenv("TERM");
    int colored = isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0 && !getenv("NO_COLOR");

    const char *status =
        (iface->flags & IFF_UP) ? "UP" : "DOWN";

    if (!colored) {
        printf("%2d) %-15s (%-10s) [%s]\n", index, iface->name, type_str(iface->type), status);
        return;
    }
    printf(
        "%2d) \033[1;%dm%-15s\033[0m "
        "(%-10s) [%s]\n",
        index,
        color,
        iface->name,
        type_str(iface->type),
        status
    );
}

/* Avoid duplicate entries from getifaddrs */
static int interface_exists(
    const char *name,
    int count
) {
    for (int i = 0; i < count; i++) {
        if (strcmp(interfaces[i].name, name) == 0)
            return i;
    }

    return -1;
}

static int compare_interfaces(
    const void *a,
    const void *b
) {
    const NetworkInterface *ia = a;
    const NetworkInterface *ib = b;

    return strcmp(ia->name, ib->name);
}

/* List and select network interface */
const char *list_and_select_interface(void) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;

    int count = 0;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return NULL;
    }

    CFArrayRef sc_interfaces =
        SCNetworkInterfaceCopyAll();

    for (ifa = ifaddr;
         ifa != NULL;
         ifa = ifa->ifa_next) {

        if (!ifa->ifa_name || !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK)
            continue;

        int existing =
            interface_exists(ifa->ifa_name, count);

        if (existing >= 0) {
            interfaces[existing].flags |= ifa->ifa_flags;
            continue;
        }

        if (count >= MAX_INTERFACES)
            break;

        size_t len = strlen(ifa->ifa_name);

        if (len == 0 || len >= MAX_NAME_LEN)
            continue;

        NetworkInterface *entry =
            &interfaces[count];

        memcpy(entry->name, ifa->ifa_name, len + 1);

        entry->flags = ifa->ifa_flags;

        entry->type = get_interface_type(
            entry->name,
            entry->flags,
            sc_interfaces
        );

        count++;
    }

    if (sc_interfaces)
        CFRelease(sc_interfaces);

    freeifaddrs(ifaddr);

    if (count == 0) {
        fprintf(stderr,
                "No network interfaces found.\n");
        return NULL;
    }

    qsort(
        interfaces,
        count,
        sizeof(interfaces[0]),
        compare_interfaces
    );

    const char *term = getenv("TERM");
    int interactive = isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0;
    if (interactive)
        printf("\033[H\033[J");
    if (interactive && !getenv("NO_COLOR"))

    printf(
        "\033[1;31m"
        "Available Network Interfaces (macOS)"
        "\033[0m\n\n"
    );

    else
        printf("Available Network Interfaces (macOS)\n\n");

    for (int i = 0; i < count; i++) {
        print_colored(i + 1, &interfaces[i]);
    }

    char input[128];

    printf("\nSelect an interface [1-%d]: ", count);
    fflush(stdout);

    if (!fgets(input, sizeof(input), stdin)) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }

    if (!strchr(input, '\n') && !feof(stdin)) {
        int ch;

        while ((ch = getchar()) != '\n' &&
               ch != EOF) {
        }

        fprintf(stderr, "Input too long.\n");
        return NULL;
    }

    char *end = NULL;

    errno = 0;

    long choice = strtol(input, &end, 10);

    if (errno == ERANGE || end == input) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }

    while (isspace((unsigned char)*end))
        end++;

    if (*end != '\0' ||
        choice < 1 ||
        choice > count) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }

    return interfaces[choice - 1].name;
}
