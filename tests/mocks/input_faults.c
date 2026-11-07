/* Inject SIGHUP exactly after the flag check, before a blocking syscall. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static void signal_before_wait(void)
{
    static int injected;
    const char *fault = getenv("NERO_TEST_FAULT");
    if (!injected && fault && strcmp(fault, "signal-before-wait") == 0) {
        injected = 1;
        fputs("SIGNAL_BEFORE_WAIT\n", stderr);
        fflush(stderr);
        raise(SIGHUP);
    }
}

ssize_t fault_read(int fd, void *buffer, size_t length)
{
    signal_before_wait();
    return read(fd, buffer, length);
}

int fault_pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                  const struct timespec *timeout, const sigset_t *mask)
{
    signal_before_wait();
    return pselect(nfds, readfds, writefds, exceptfds, timeout, mask);
}

#define read fault_read
#define pselect fault_pselect
#include "../../src/cli/input.c"
