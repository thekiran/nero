#ifndef NERO_WIN_COMPAT_H
#define NERO_WIN_COMPAT_H

/* Windows compatibility layer for the Nero CLI (MinGW-w64 / GCC).
 *
 * Included FIRST in every Windows translation unit so the POSIX-named
 * interfaces the shared sources rely on (termios, poll, sigaction, ioctl,
 * winsock errno semantics) resolve to Win32 equivalents. On non-Windows
 * builds this header is never included and the shared sources compile
 * against real POSIX headers unchanged.
 *
 * Scope notes:
 *  - Sockets: winsock2 with WSAStartup performed by nero_windows_init().
 *    Socket errno checks in shared code compare errno against POSIX codes,
 *    so socket calls are wrapped to translate WSAGetLastError() into errno
 *    and the POSIX error macros are remapped to their WSAE* values.
 *  - poll(): sockets go through WSAPoll; STDIN goes through
 *    WaitForSingleObject because WSAPoll only accepts socket handles.
 *    The struct keeps a signed fd so the shared "fd = -1" convention
 *    (ignore this slot) keeps working.
 *  - termios: the two shared consumers (terminal session, port scanner)
 *    only toggle ICANON/ECHO, so the shim maps those bits onto console
 *    input modes. Raw mode also enables ENABLE_VIRTUAL_TERMINAL_INPUT so
 *    arrow keys arrive as the ANSI sequences the line editor decodes.
 *  - ioctl: only TIOCGWINSZ is used by shared code.
 *  - sigaction: enough surface for the session signal handlers (query +
 *    install/restore of simple handlers); masks are accepted but unused.
 *  - close() only ever closes sockets in the sources that include this
 *    layer, so it maps to closesocket().
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* WSAPoll, GetAdaptersAddresses */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <errno.h>
#include <io.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

/* unistd.h on MinGW provides read/isatty as oldname mappings; force the
 * safe CRT names so no deprecation diagnostics can appear. */
#undef isatty
#define isatty(fd) _isatty(fd)

typedef long long nero_ssize_t;
#if !defined(_SSIZE_T_DEFINED)
typedef nero_ssize_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* ------------------------------------------------------------------ */
/* Socket errno translation                                            */
/* ------------------------------------------------------------------ */

/* Shared code checks errno after socket calls. Wrappers below copy
 * WSAGetLastError() into errno right after a failing call, and these
 * macros make the comparisons compare like-for-like. errno.h is already
 * included above, so re-including it later in the TU is a guarded no-op. */
#undef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#undef EINPROGRESS
#define EINPROGRESS WSAEWOULDBLOCK
#undef EALREADY
#define EALREADY WSAEALREADY
#undef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#undef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#undef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#undef ENETUNREACH
#define ENETUNREACH WSAENETUNREACH
#undef EMFILE
#define EMFILE WSAEMFILE
#undef ENFILE
#define ENFILE WSAENFILE
#undef ENOBUFS
#define ENOBUFS WSAENOBUFS

/* Every close() in the socket-facing sources targets a socket. */
#define close closesocket

static inline int nero_socket(int domain, int type, int protocol)
{
    SOCKET handle = socket(domain, type, protocol);
    if (handle == INVALID_SOCKET) {
        errno = WSAGetLastError();
        return -1;
    }
    /* Win32 handles have 32-bit significance; the CLI keeps bounded socket
     * counts, so the narrowing cast is safe for the select()/fd_set paths. */
    return (int)handle;
}
#define socket nero_socket

static inline int nero_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int result = connect((SOCKET)fd, address, (int)length);
    if (result < 0)
        errno = WSAGetLastError();
    return result;
}
#define connect nero_connect

static inline int nero_getsockopt(int fd, int level, int option,
                                  void *value, socklen_t *length)
{
    int int_length = (int)*length;
    int result = getsockopt((SOCKET)fd, level, option, (char *)value, &int_length);
    *length = (socklen_t)int_length;
    return result;
}
#define getsockopt nero_getsockopt

/* ------------------------------------------------------------------ */
/* poll(): WSAPoll for sockets, WaitForSingleObject for stdin          */
/* ------------------------------------------------------------------ */

/* Winsock supplies the WSAPoll event constants. Keep those values: POSIX
 * numeric values are different and cannot be passed through to WSAPoll.
 * Its struct pollfd stores SOCKET; the shared CLI uses signed descriptors,
 * so give our translation structure a distinct name. */
#define pollfd nero_pollfd

typedef unsigned long nfds_t;

struct pollfd {
    int fd;    /* signed: -1 marks a removed slot, as the shared code expects */
    short events;
    short revents;
};

static inline int poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
    if (count == 1 && fds[0].fd == STDIN_FILENO) {
        /* WSAPoll refuses non-socket handles; console input handles and
         * pipes are waitable kernel objects, so wait on the OS handle. */
        HANDLE input = (HANDLE)_get_osfhandle(fds[0].fd);
        if (input == INVALID_HANDLE_VALUE) {
            fds[0].revents = POLLNVAL;
            return 1;
        }
        DWORD wait = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
        DWORD signaled = WaitForSingleObject(input, wait);
        if (signaled == WAIT_FAILED)
            return -1;
        if (signaled == WAIT_TIMEOUT) {
            fds[0].revents = 0;
            return 0;
        }
        fds[0].revents = POLLIN; /* EOF surfaces as a zero-byte read later */
        return 1;
    }

    if (count == 0)
        return 0;
    if (count > 512)
        return -1; /* shared callers batch at most MAX_BATCH (512) sockets */

    WSAPOLLFD batch[512];
    for (unsigned long i = 0; i < count; ++i) {
        batch[i].fd = fds[i].fd < 0 ? INVALID_SOCKET : (SOCKET)fds[i].fd;
        batch[i].events = fds[i].events;
        batch[i].revents = 0;
    }
    int ready = WSAPoll(batch, (ULONG)count, timeout_ms);
    if (ready == SOCKET_ERROR)
        return -1;
    for (unsigned long i = 0; i < count; ++i) {
        if (fds[i].fd < 0) {
            fds[i].revents = 0;
            continue;
        }
        fds[i].revents = batch[i].revents;
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* Terminal size (TIOCGWINSZ) and termios via console modes            */
/* ------------------------------------------------------------------ */

#define TIOCGWINSZ 0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static inline int ioctl(int fd, int request, struct winsize *size)
{
    if (request != TIOCGWINSZ) {
        errno = EINVAL;
        return -1;
    }
    HANDLE output = (HANDLE)_get_osfhandle(fd);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (output == INVALID_HANDLE_VALUE ||
        !GetConsoleScreenBufferInfo(output, &info)) {
        errno = ENOTTY;
        return -1;
    }
    size->ws_col = (unsigned short)(info.srWindow.Right - info.srWindow.Left + 1);
    size->ws_row = (unsigned short)(info.srWindow.Bottom - info.srWindow.Top + 1);
    size->ws_xpixel = 0;
    size->ws_ypixel = 0;
    return 0;
}

/* Local flag bits used by the shared sources. Values are local to Nero;
 * nothing outside this header interprets them numerically. */
#define ICANON 0x0002
#define ECHO 0x0008

#define NCCS 20
#define VMIN 16
#define VTIME 17

#define TCSANOW 0
#define TCSADRAIN 1

struct termios {
    DWORD nero_console_mode; /* snapshot of the full console input mode */
    unsigned long c_lflag;
    unsigned char c_cc[NCCS];
};

static inline int tcgetattr(int fd, struct termios *settings)
{
    HANDLE input = (HANDLE)_get_osfhandle(fd);
    DWORD mode = 0;
    if (settings == NULL || input == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &mode)) {
        errno = ENOTTY;
        return -1;
    }
    settings->nero_console_mode = mode;
    settings->c_lflag = 0;
    if (mode & ENABLE_ECHO_INPUT)
        settings->c_lflag |= ECHO;
    if (mode & ENABLE_LINE_INPUT)
        settings->c_lflag |= ICANON;
    memset(settings->c_cc, 0, sizeof(settings->c_cc));
    return 0;
}

static inline int tcsetattr(int fd, int when, const struct termios *settings)
{
    (void)when; /* Both shared callers are immediate-mode semantics. */
    if (settings == NULL) {
        errno = EINVAL;
        return -1;
    }
    HANDLE input = (HANDLE)_get_osfhandle(fd);
    if (input == INVALID_HANDLE_VALUE) {
        errno = ENOTTY;
        return -1;
    }
    DWORD mode = settings->nero_console_mode;
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                     ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_PROCESSED_INPUT);
    if (settings->c_lflag & ECHO)
        mode |= ENABLE_ECHO_INPUT;
    if (settings->c_lflag & ICANON)
        mode |= ENABLE_LINE_INPUT;
    /* Processed input stays on so Ctrl+C reaches the signal handler exactly
     * like the POSIX builds; raw mode additionally requests VT input so the
     * line editor receives ANSI arrow-key sequences. */
    mode |= ENABLE_PROCESSED_INPUT;
    if ((settings->c_lflag & (ICANON | ECHO)) != (ICANON | ECHO))
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(input, mode)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sigaction: install/query simple handlers                            */
/* ------------------------------------------------------------------ */

typedef unsigned long long sigset_t;

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
};

#define sigemptyset(set) (*(set) = 0)

static inline int sigaction(int signal_number, const struct sigaction *action,
                            struct sigaction *previous)
{
    /* Handlers registered through this shim only; table tracks the current
     * handler per signal because the Win32 signal() cannot query. NULL in
     * the table means SIG_DFL. */
    static void (*installed[NSIG])(int);

    if (signal_number <= 0 || signal_number >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (previous != NULL)
        previous->sa_handler = installed[signal_number];
    if (action != NULL) {
        void (*handler)(int) = action->sa_handler ? action->sa_handler : SIG_DFL;
        if (signal(signal_number, handler) == SIG_ERR) {
            errno = EINVAL;
            return -1;
        }
        installed[signal_number] = action->sa_handler;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Session startup: winsock + UTF-8 + VT output                        */
/* ------------------------------------------------------------------ */

/* Called once from main() on Windows builds (see src/main.c). */
static inline void nero_windows_init(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        /* ANSI escape sequences (the shared renderer's only dialect) and
         * UTF-8 output so the shared logo glyphs survive the code page. */
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
    }
}

#endif /* NERO_WIN_COMPAT_H */
