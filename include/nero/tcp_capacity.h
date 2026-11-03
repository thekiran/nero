#ifndef NERO_TCP_CAPACITY_H
#define NERO_TCP_CAPACITY_H
#include <stddef.h>
#include <signal.h>
int nero_tcp_test(char **argv, size_t argc, volatile sig_atomic_t *cancel);
#endif
