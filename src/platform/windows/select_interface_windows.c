/* Windows interface adapter.
 * Enumerates local network interfaces using GetAdaptersAddresses(), maps
 * IANA interface types to the same labels and colors as the POSIX adapters,
 * and returns the name selected by the user. Selection does not configure
 * the interface or generate network traffic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include "nero_win_compat.h" /* first: winsock2/winsock order, isatty, errno */
#include <iphlpapi.h>
#else
#error "This adapter is Windows-only; build the POSIX adapter instead."
#endif

#include "nero/select_interface.h"

#define MAX_INTERFACES 512
#define MAX_NAME_LEN 64

/* Same colors and labels as the macOS/Linux adapters. */
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

/* One link-layer record per adapter, like AF_LINK on the POSIX adapters. */
static int enumerate_interfaces(void)
{
    ULONG buffer_size = 15 * 1024; /* recommended starting size (MSDN) */
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_UNICAST;
    IP_ADAPTER_ADDRESSES *head = NULL;
    ULONG result;
    int count = 0;

    /* Adapter sizes change between calls; retry the documented way. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        head = malloc(buffer_size);
        if (head == NULL) {
            return -1;
        }
        result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, head, &buffer_size);
        if (result == ERROR_SUCCESS)
            break;
        free(head);
        head = NULL;
        if (result != ERROR_BUFFER_OVERFLOW)
            return -1;
    }
    if (head == NULL)
        return -1;

    for (const IP_ADAPTER_ADDRESSES *adapter = head; adapter != NULL;
         adapter = adapter->Next) {
        if (adapter->AdapterName == NULL || adapter->FriendlyName == NULL)
            continue;

        char name[MAX_NAME_LEN];
        if (!WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                 name, sizeof(name), NULL, NULL) ||
            name[0] == '\0') {
            continue;
        }
        if (find_interface(name, count) >= 0 || count >= MAX_INTERFACES)
            continue;

        InterfaceEntry *entry = &interfaces[count];
        memcpy(entry->name, name, strlen(name) + 1);
        switch (adapter->IfType) {
        case IF_TYPE_SOFTWARE_LOOPBACK:
            entry->type = TYPE_LOOPBACK;
            break;
        case IF_TYPE_IEEE80211:
            entry->type = TYPE_WIRELESS;
            break;
        case IF_TYPE_ETHERNET_CSMACD:
            entry->type = TYPE_ETHERNET;
            break;
        default:
            entry->type = TYPE_UNKNOWN;
            break;
        }
        ++count;
    }

    free(head);
    return count;
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
    return strcmp(((const InterfaceEntry *)left)->name,
                  ((const InterfaceEntry *)right)->name);
}

/* Redirected output and NO_COLOR receive plain text. Windows consoles have
 * no TERM convention; an interactive console is ANSI-capable once
 * nero_windows_init() enabled VT processing. */
static void print_interfaces(int count)
{
    int interactive = isatty(1) &&
        (!getenv("TERM") || strcmp(getenv("TERM"), "dumb") != 0);
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
            printf("%2d) \033[1;%dm%-15s\033[0m (%s)\n", i + 1,
                   type_color(entry->type), entry->name, type_label(entry->type));
        else
            printf("%2d) %-15s (%s)\n", i + 1, entry->name, type_label(entry->type));
    }
}

/* Accept one complete decimal integer in [1, count], optionally surrounded
 * by whitespace; anything else is rejected rather than partially parsed.
 * Mirrors the POSIX adapters byte for byte. */
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
    long choice = strtol(input, &end, 10);
    if (end == input) {
        fprintf(stderr, "Invalid selection.\n");
        return -1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
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
    if (count < 0) {
        fprintf(stderr, "Interface enumeration failed.\n");
        return NULL;
    }
    if (count == 0) {
        fprintf(stderr, "No network interfaces found.\n");
        return NULL;
    }

    qsort(interfaces, (size_t)count, sizeof(interfaces[0]), compare_interfaces);
    print_interfaces(count);

    int index = read_selection(count);
    return index < 0 ? NULL : interfaces[index].name;
}
