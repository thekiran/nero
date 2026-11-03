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
    CFRelease(all); /* SCNetworkInterfaceCopyAll follows the Copy rule. */
}

static const char *type_label(InterfaceType type)
{
    switch (type) {
    case TYPE_ETHERNET:
        return "Ethernet";
    case TYPE_WIRELESS:
        return "Wireless";
    case TYPE_LOOPBACK:
        return "Loopback";
    default:
        return "Unknown";
    }
}

static int type_color(InterfaceType type)
{
    switch (type) {
    case TYPE_ETHERNET:
        return COLOR_BLUE;
    case TYPE_WIRELESS:
        return COLOR_GREEN;
    case TYPE_LOOPBACK:
        return COLOR_YELLOW;
    default:
        return COLOR_RED;
    }
}

static int compare_interfaces(const void *left, const void *right)
{
    return strcmp(((const InterfaceEntry *)left)->name, ((const InterfaceEntry *)right)->name);
}

/* Redirected output and NO_COLOR receive plain text, matching the CLI's
 * rendering capability rules. */
static void print_interfaces(int count)
{
    const char *term = getenv("TERM");
    int interactive = isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0;
    int colored = interactive && getenv("NO_COLOR") == NULL;

    if (interactive)
        printf("\033[H\033[J");
    if (colored)
        printf("\033[1;31mAvailable Network Interfaces:\033[0m\n\n");
    else
        printf("Available Network Interfaces:\n\n");

    for (int i = 0; i < count; ++i) {
        const InterfaceEntry *entry = &interfaces[i];
        if (colored)
            printf("%2d) \033[1;%dm%-15s\033[0m (%s)\n", i + 1, type_color(entry->type),
                   entry->name, type_label(entry->type));
        else
            printf("%2d) %-15s (%s)\n", i + 1, entry->name, type_label(entry->type));
    }
}

/* Accept one complete decimal integer in [1, count], optionally surrounded
 * by whitespace; anything else is rejected rather than partially parsed. */
static int read_selection(int count)
{
    char input[128];

    printf("\nSelect an interface [1-%d]: ", count);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        fprintf(stderr, "Invalid selection.\n");
        return -1;
    }
    if (strchr(input, '\n') == NULL && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {
        }
        fprintf(stderr, "Input too long.\n");
        return -1;
    }

    char *end = NULL;
    errno = 0;
    long choice = strtol(input, &end, 10);
    if (errno == ERANGE || end == input) {
        fprintf(stderr, "Invalid selection.\n");
        return -1;
    }
    while (isspace((unsigned char)*end))
        ++end;
    if (*end != '\0' || choice < 1 || choice > count) {
        fprintf(stderr, "Invalid selection.\n");
        return -1;
    }
    return (int)choice - 1;
}

const char *list_and_select_interface(void)
{
    int count = enumerate_interfaces();
    if (count < 0)
        return NULL;
    if (count == 0) {
        fprintf(stderr, "No network interfaces found.\n");
        return NULL;
    }

    classify_interfaces(count);
    qsort(interfaces, (size_t)count, sizeof(interfaces[0]), compare_interfaces);
    print_interfaces(count);

    int index = read_selection(count);
    return index < 0 ? NULL : interfaces[index].name;
}
