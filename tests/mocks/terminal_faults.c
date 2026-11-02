/* Compile the production terminal with test-only termios fault injection. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

static int fault_getattr(int fd, struct termios *settings)
{
    const char *fault = getenv("NERO_TEST_FAULT");
    if (fault && strcmp(fault, "getattr") == 0) { errno = EIO; return -1; }
    return tcgetattr(fd, settings);
}

static int fault_setattr(int fd, int action, const struct termios *settings)
{
    static int calls;
    const char *fault = getenv("NERO_TEST_FAULT");
    ++calls;
    if (fault && calls == 1 && strcmp(fault, "setattr") == 0) { errno = EIO; return -1; }
    if (fault && calls == 2 && strcmp(fault, "restore-eintr") == 0) { errno = EINTR; return -1; }
    return tcsetattr(fd, action, settings);
}

#define tcgetattr fault_getattr
#define tcsetattr fault_setattr
#include "../../src/cli/terminal.c"
