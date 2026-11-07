/* Offline Win32 boundary tests: synthetic handles, mocked console state. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

static SOCKET expected_handle, observed_handle;
static DWORD initial_mode, observed_mode;
static int set_mode_fails, poll_fails;
static SOCKET fake_socket(int domain, int type, int protocol)
{ (void)domain; (void)type; (void)protocol; return expected_handle; }
static int fake_connect(SOCKET handle, const struct sockaddr *address, int length)
{ (void)address; (void)length; observed_handle = handle; return 0; }
static int fake_getsockopt(SOCKET handle, int level, int option, char *value, int *length)
{
    (void)level; (void)option;
    observed_handle = handle;
    *(int *)value = 0;
    *length = sizeof(int);
    return 0;
}
static int fake_poll(WSAPOLLFD *fds, ULONG count, INT timeout)
{
    (void)timeout;
    if (poll_fails) { WSASetLastError(WSAENOBUFS); return SOCKET_ERROR; }
    observed_handle = fds[0].fd;
    for (ULONG i = 0; i < count; ++i) fds[i].revents = POLLOUT;
    return 1;
}
static intptr_t fake_osfhandle(int fd) { (void)fd; return 123; }
static BOOL fake_getmode(HANDLE handle, LPDWORD mode)
{ (void)handle; *mode = initial_mode; return TRUE; }
static BOOL fake_setmode(HANDLE handle, DWORD mode)
{ (void)handle; observed_mode = mode; return !set_mode_fails; }
#define NERO_WIN_SOCKET fake_socket
#define NERO_WIN_CONNECT fake_connect
#define NERO_WIN_GETSOCKOPT fake_getsockopt
#define NERO_WIN_WSAPOLL fake_poll
#define NERO_WIN_OSFHANDLE fake_osfhandle
#define NERO_WIN_GETMODE fake_getmode
#define NERO_WIN_SETMODE fake_setmode
#include "../../src/platform/windows/nero_win_compat.h"
#include "nero/socket_handle.h"
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return 1; \
} } while (0)
_Static_assert(sizeof(nero_socket_t) == sizeof(SOCKET), "socket handle narrowing");
_Static_assert(sizeof(((struct pollfd *)0)->fd) == sizeof(SOCKET), "poll handle narrowing");

int main(void)
{
    expected_handle = ((SOCKET)1 << 40) | 123;
    CHECK(expected_handle > (SOCKET)INT_MAX);
    nero_socket_t handle = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(handle == expected_handle);
    CHECK(connect(handle, NULL, 0) == 0 && observed_handle == handle);
    int error = -1;
    socklen_t length = sizeof(error);
    CHECK(getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &length) == 0);
    CHECK(observed_handle == handle && error == 0);
    fd_set set;
    FD_ZERO(&set); FD_SET(handle, &set);
    CHECK(FD_ISSET(handle, &set));
    CHECK(set.fd_array[0] == handle);
    struct pollfd fds[2] = {{handle, POLLOUT, 0}, {INVALID_SOCKET, POLLOUT, 0}};
    CHECK(nero_socket_poll(fds, 2, 0) == 1 && observed_handle == handle);
    CHECK(fds[0].revents == POLLOUT && fds[1].revents == 0);
    fds[0].fd = 0; /* A zero SOCKET must not be treated as CRT stdin. */
    CHECK(nero_socket_poll(fds, 1, 0) == 1 && observed_handle == 0);
    poll_fails = 1;
    CHECK(nero_socket_poll(fds, 1, 0) == -1 && errno == WSAENOBUFS);
    poll_fails = 0;
    expected_handle = INVALID_SOCKET;
    CHECK(socket(AF_INET, SOCK_STREAM, 0) == INVALID_SOCKET);

    for (unsigned bits = 0; bits < 16; ++bits) {
        initial_mode = ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE;
        if (bits & 1) initial_mode |= ENABLE_LINE_INPUT;
        if (bits & 2) initial_mode |= ENABLE_ECHO_INPUT;
        if (bits & 4) initial_mode |= ENABLE_PROCESSED_INPUT;
        if (bits & 8) initial_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        struct termios snapshot, raw;
        CHECK(tcgetattr(0, &snapshot) == 0);
        raw = snapshot;
        raw.c_lflag &= ~(ICANON | ECHO);
        CHECK(tcsetattr(0, TCSANOW, &raw) == 0);
        CHECK(tcsetattr(0, TCSANOW, &snapshot) == 0);
        CHECK(observed_mode == initial_mode);
        CHECK(tcsetattr(0, TCSANOW, &snapshot) == 0 && observed_mode == initial_mode);
    }
    struct termios snapshot;
    CHECK(tcgetattr(0, &snapshot) == 0);
    set_mode_fails = 1;
    CHECK(tcsetattr(0, TCSANOW, &snapshot) == -1 && errno == EPERM);
    puts("PASS: pointer-sized SOCKET, fd_set/poll, invalid/zero handles and exact console restoration");
    return 0;
}
