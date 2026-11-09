/* Deterministic kernel metadata for the production Linux adapter. No network
 * operations. Native enumeration is tested separately on Linux CI runners. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
static struct ifaddrs entries[3];
DIR *__wrap_opendir(const char *path)
{
    if (strcmp(path, "/sys/class/net") != 0)
        abort();
    errno = ENOENT;
    return NULL;
}
int __wrap_getifaddrs(struct ifaddrs **out)
{
    memset(entries, 0, sizeof entries);
    entries[0].ifa_name = "fixture0";
    entries[0].ifa_flags = IFF_LOOPBACK;
    entries[0].ifa_next = &entries[1];
    entries[1].ifa_name = "fixture0";
    entries[1].ifa_next = &entries[2];
    entries[2].ifa_name = "fixture1";
    *out = entries;
    return 0;
}
void __wrap_freeifaddrs(struct ifaddrs *p)
{
    if (p != entries)
        abort();
}
