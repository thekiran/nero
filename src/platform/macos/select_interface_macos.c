#include <stdio.h>
#include <stdlib.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include "nero/select_interface.h"
#include <string.h>

#define MAX_INTERFACES 128
#define MAX_NAME_LEN 64

// ANSI color codes
enum {
    COLOR_RESET = 0,
    COLOR_BLUE = 34,    // Ethernet
    COLOR_GREEN = 32,   // Wireless
    COLOR_YELLOW = 33,  // Loopback
    COLOR_RED = 31      // Other
};

// Human-readable interface type
static const char* type_str(int type) {
    switch (type) {
        case 1:   return "Ethernet";
        case 801: return "Wireless";
        case 772: return "Loopback";
        default:  return "Unknown";
    }
}

// Print a colored interface row
static void print_colored(int idx, const char *name, const char *desc, int type) {
    int color;
    if (type == 1)        color = COLOR_BLUE;   // Ethernet
    else if (type == 801) color = COLOR_GREEN;  // Wireless
    else if (type == 772) color = COLOR_YELLOW; // Loopback
    else                  color = COLOR_RED;    // Other

    printf("%2d) \033[1;%dm%-15s\033[0m (%s)\n", idx, color, name, desc);
}

// List interfaces and read the selection
const char *list_and_select_interface(void) {
    static char names[MAX_INTERFACES][MAX_NAME_LEN];
    static int types[MAX_INTERFACES];
    int count = 0;

    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) {
        perror("getifaddrs");
        return NULL;
    }
    for (const struct ifaddrs *it = addresses; it; it = it->ifa_next) {
        if (!it->ifa_name || !it->ifa_addr ||
            it->ifa_addr->sa_family != AF_LINK) continue;
        size_t length = strlen(it->ifa_name);
        if (length == 0 || length >= MAX_NAME_LEN) continue;
        int duplicate = 0;
        for (int i = 0; i < count; ++i)
            if (strcmp(names[i], it->ifa_name) == 0) duplicate = 1;
        if (duplicate) continue;
        if (count == MAX_INTERFACES) {
            fprintf(stderr, "Interface list truncated to %d entries.\n", MAX_INTERFACES);
            break;
        }
        memcpy(names[count], it->ifa_name, length + 1);
        /* AF_LINK does not reliably distinguish Ethernet from Wi-Fi. */
        types[count] = (it->ifa_flags & IFF_LOOPBACK) ? 772 : -1;
        count++;
    }
    freeifaddrs(addresses);

    if (count == 0) {
        fprintf(stderr, "No network interfaces found.\n");
        return NULL;
    }

    // Clear the screen
    printf("\033[H\033[J");
    printf("\033[1;31mAvailable Network Interfaces:\033[0m\n\n");

    for (int i = 0; i < count; ++i) {
        print_colored(i + 1, names[i], type_str(types[i]), types[i]);
    }

    int choice = 0;
    printf("\nSelect an interface [1-%d]: ", count);
    if (scanf("%d", &choice) != 1 || choice < 1 || choice > count) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }

    return names[choice - 1];
}
