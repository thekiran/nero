/* Compile the real adapter against deterministic, read-only fixtures. */
#include <stdio.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct ifaddrs entries[132];
static struct sockaddr link_address;
static char names[132][80];
static int freed;

static int fixture_getifaddrs(struct ifaddrs **result)
{
    const char *mode = getenv("INTERFACE_FIXTURE");
    if (mode && strcmp(mode, "error") == 0) {
        errno = ENOMEM;
        return -1;
    }
    if (mode && strcmp(mode, "empty") == 0) {
        *result = NULL;
        return 0;
    }
    link_address.sa_family = AF_LINK;
    for (int i = 0; i < 132; ++i) {
        snprintf(names[i], sizeof(names[i]), "fixture%d", i);
        entries[i].ifa_name = names[i];
        entries[i].ifa_addr = &link_address;
        entries[i].ifa_next = i < 131 ? &entries[i + 1] : NULL;
    }
    if (!mode || strcmp(mode, "limit") != 0) {
        entries[0].ifa_flags = IFF_LOOPBACK;
        entries[1].ifa_name = names[0]; /* duplicate */
        entries[2].ifa_addr = NULL;
        entries[3].ifa_name = NULL;
        memset(names[4], 'x', sizeof(names[4]) - 1); /* too long */
        entries[5].ifa_next = NULL;
    }
    *result = entries;
    return 0;
}

static void fixture_freeifaddrs(struct ifaddrs *addresses)
{
    (void)addresses;
    freed = 1;
}

#define getifaddrs fixture_getifaddrs
#define freeifaddrs fixture_freeifaddrs
#include "../platform/select_interface_macos.c"

int main(void)
{
    const char *name = list_and_select_interface();
    const char *mode = getenv("INTERFACE_FIXTURE");
    if ((!mode || strcmp(mode, "error") != 0) && !freed) return 2;
    if (!name) return 1;
    printf("SELECTED:%s\n", name);
    return 0;
}
