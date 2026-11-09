#define _GNU_SOURCE
#include "nero/select_interface.h"
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int fail_addresses, freed;
static struct ifaddrs nodes[4];
DIR *__wrap_opendir(const char *path)
{
    CHECK(strcmp(path, "/sys/class/net") == 0);
    errno = ENOENT;
    return NULL;
}
int __wrap_getifaddrs(struct ifaddrs **out)
{
    if (fail_addresses) {
        errno = EIO;
        return -1;
    }
    memset(nodes, 0, sizeof nodes);
    nodes[0].ifa_name = "lo";
    nodes[0].ifa_flags = IFF_LOOPBACK;
    nodes[0].ifa_next = &nodes[1];
    nodes[1].ifa_name = "lo";
    nodes[1].ifa_flags = IFF_LOOPBACK;
    nodes[1].ifa_next = &nodes[2];
    nodes[2].ifa_name = "eth-fixture";
    nodes[2].ifa_next = &nodes[3];
    nodes[3].ifa_name = NULL;
    *out = nodes;
    return 0;
}
void __wrap_freeifaddrs(struct ifaddrs *p)
{
    CHECK(p == nodes);
    freed++;
}
static const char *choose(const char *text)
{
    int input[2];
    CHECK(pipe(input) == 0);
    size_t size = strlen(text);
    CHECK(write(input[1], text, size) == (ssize_t)size);
    CHECK(close(input[1]) == 0);
    CHECK(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    CHECK(close(input[0]) == 0);
    clearerr(stdin);
    return list_and_select_interface();
}
int main(void)
{
    const char *name = choose("2\n");
    CHECK(name && strcmp(name, "eth-fixture") == 0 && freed == 1);
    CHECK(choose("3\n") == NULL && freed == 2); /* duplicate lo is not another interface */
    CHECK(choose("1junk\n") == NULL && freed == 3);
    fail_addresses = 1;
    CHECK(choose("1\n") == NULL && freed == 3);
    puts(
        "PASS sysfs fallback, duplicate/NULL names, invalid input, getifaddrs cleanup and failure");
    return 0;
}
