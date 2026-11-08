/* Deterministic PTY smoke test for live early port selection (macOS + Linux).
 *
 * Drives the real portscanner() through a genuine pseudo-terminal so the live
 * discovery animation, its key handling and the open-port menu all run exactly
 * as they do for a user. There is NO real network I/O: every socket call is a
 * static mock, so nothing is sent to loopback or any third-party address.
 *
 * Determinism (no flaky sleeps): the production scanner carries a test-only
 * NERO_SCAN_TEST_HOOKS signalling path that writes lifecycle tags (SCAN-START,
 * OPEN, ENTER-STOP, ENTER-IGNORED) to a sync pipe, and the mock clock is frozen
 * so a pending scan never times out on its own. The controlling parent blocks
 * on those tags instead of guessing durations, reveals the single open port on
 * demand through a control pipe, and only then presses Enter. If anything
 * deviates the test fails loudly; it never silently retries.
 *
 * Windows native console behaviour is validated separately (tests/windows and
 * the MinGW CI leg), never here. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define NERO_SCAN_TEST_HOOKS

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "nero/socket_handle.h"

/* ------------------------- mock socket layer ------------------------- */
#define FAKE_HANDLES 4096
#define HANDLE_BASE ((nero_socket_t)6000)
static pthread_mutex_t mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used, port; } handles[FAKE_HANDLES];
static int active_handles;
static int fixture_open_port;   /* the single port revealed open, or 0 */
static int fixture_all_closed;  /* every connect refused -> scan completes */
static int ctrl_read_fd = -1;   /* parent -> child reveal pipe (non-blocking) */
static atomic_int reveal_open;  /* set once the parent asks to reveal the port */

static int idx_of(nero_socket_t fd) { return (int)(fd - HANDLE_BASE); }

static nero_socket_t fake_socket(int d, int t, int p) {
    (void)d; (void)t; (void)p;
    pthread_mutex_lock(&mock_mutex);
    for (int i = 0; i < FAKE_HANDLES; ++i) if (!handles[i].used) {
        handles[i].used = 1; handles[i].port = 0; active_handles++;
        pthread_mutex_unlock(&mock_mutex);
        return HANDLE_BASE + i;
    }
    pthread_mutex_unlock(&mock_mutex);
    errno = EMFILE; return NERO_INVALID_SOCKET;
}
static int fake_close(nero_socket_t fd) {
    int i = idx_of(fd);
    pthread_mutex_lock(&mock_mutex);
    if (i >= 0 && i < FAKE_HANDLES && handles[i].used) { handles[i].used = 0; active_handles--; }
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}
static int fake_fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int fake_connect(nero_socket_t fd, const struct sockaddr *a, socklen_t l) {
    (void)l;
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)a;
    if (a->sa_family != AF_INET || (ntohl(in->sin_addr.s_addr) >> 24) != 127) {
        fputs("FAIL fixture attempted a non-loopback connect\n", stderr); abort();
    }
    int port = ntohs(in->sin_port);
    int i = idx_of(fd);
    pthread_mutex_lock(&mock_mutex);
    if (i >= 0 && i < FAKE_HANDLES) handles[i].port = port;
    pthread_mutex_unlock(&mock_mutex);
    if (fixture_all_closed) { errno = ECONNREFUSED; return -1; }
    errno = EINPROGRESS; return -1; /* pending; fake_poll resolves the open port */
}
static void drain_control(void) {
    if (ctrl_read_fd < 0) return;
    char b;
    while (read(ctrl_read_fd, &b, 1) == 1) atomic_store(&reveal_open, 1);
}
static int fake_poll(struct pollfd *fds, nfds_t n, int timeout) {
    drain_control();
    int reveal = atomic_load(&reveal_open), ready = 0;
    pthread_mutex_lock(&mock_mutex);
    for (nfds_t s = 0; s < n; ++s) {
        fds[s].revents = 0;
        if (fds[s].fd == NERO_INVALID_SOCKET) continue;
        int i = idx_of(fds[s].fd);
        if (reveal && fixture_open_port && i >= 0 && i < FAKE_HANDLES &&
            handles[i].used && handles[i].port == fixture_open_port) {
            fds[s].revents = POLLOUT; ready++;
        }
    }
    pthread_mutex_unlock(&mock_mutex);
    /* Honour the caller's wait only for CPU courtesy; correctness comes from the
     * interrupt flag checked each iteration, never from this duration. */
    if (!ready && timeout > 0) {
        struct timespec t = { timeout / 1000, (long)(timeout % 1000) * 1000000L };
        nanosleep(&t, NULL);
    }
    return ready;
}
static int fake_getsockopt(nero_socket_t fd, int lvl, int opt, void *val, socklen_t *len) {
    (void)fd; (void)lvl; (void)opt; (void)len; *(int *)val = 0; return 0; /* SO_ERROR == success */
}
static int fake_clock(clockid_t c, struct timespec *t) {
    (void)c; t->tv_sec = 1000; t->tv_nsec = 0; return 0; /* frozen: deadlines never expire */
}
static ssize_t fake_send(nero_socket_t fd, const void *b, size_t n, int f) {
    (void)fd; (void)b; (void)f; return (ssize_t)n;
}
static ssize_t fake_recv(nero_socket_t fd, void *b, size_t n, int f) {
    (void)fd; (void)b; (void)n; (void)f; return 0; /* no banner bytes */
}
#undef nero_socket_poll
#define socket fake_socket
#define connect fake_connect
#define getsockopt fake_getsockopt
#define close fake_close
#define nero_socket_poll fake_poll
#define clock_gettime fake_clock
#define fcntl fake_fcntl
#define send fake_send
#define recv fake_recv
#include "../../src/modules/portscanner.c"
#undef socket
#undef connect
#undef getsockopt
#undef close
#undef nero_socket_poll
#undef clock_gettime
#undef fcntl
#undef send
#undef recv

/* --------------------------- test harness --------------------------- */
enum { SC_EARLY_SELECT, SC_ESC_CANCEL, SC_NO_ENTER_COMPLETE };

static char out_buf[1 << 18];
static size_t out_len;

static void absorb(const char *b, ssize_t r) {
    for (ssize_t i = 0; i < r && out_len < sizeof out_buf - 1; ++i) out_buf[out_len++] = b[i];
    out_buf[out_len] = '\0';
}

/* Drain the pty master while waiting either for a specific sync tag (expected
 * != NULL) or for the child to exit (expected == NULL). Fails loudly on an
 * unexpected tag, premature exit or timeout. Returns the child exit status when
 * waiting for exit. No fixed-duration wait decides correctness. */
static int pump(int master, int sync_r, const char *expected, pid_t pid) {
    char line[64]; size_t ll = 0;
    time_t deadline = time(NULL) + 25;
    for (;;) {
        struct pollfd p[2] = { { master, POLLIN, 0 }, { sync_r, POLLIN, 0 } };
        poll(p, 2, 100);
        if (p[0].revents & POLLIN) {
            char b[2048]; ssize_t r = read(master, b, sizeof b);
            if (r > 0) absorb(b, r);
        }
        if (p[1].revents & POLLIN) {
            char c; ssize_t r;
            while ((r = read(sync_r, &c, 1)) == 1) {
                if (c == '\n') {
                    line[ll] = '\0'; ll = 0;
                    if (expected) {
                        if (strcmp(line, expected) != 0) {
                            fprintf(stderr, "FAIL expected sync '%s' but got '%s'\n", expected, line);
                            exit(1);
                        }
                        return 0;
                    }
                } else if (ll < sizeof line - 1) line[ll++] = c;
            }
        }
        if (pid > 0) {
            int st; pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) {
                /* Drain every trailing byte the child flushed on exit. Two
                 * consecutive quiet polls (no data) end it; POLLHUP alone must
                 * not stop a drain while bytes remain buffered in the pty. */
                int quiet = 0;
                while (quiet < 2) {
                    struct pollfd q = { master, POLLIN, 0 };
                    poll(&q, 1, 50);
                    char b[2048]; ssize_t r = read(master, b, sizeof b);
                    if (r > 0) { absorb(b, r); quiet = 0; }
                    else quiet++;
                }
                if (expected) {
                    fprintf(stderr, "FAIL child exited before sync '%s'\n", expected);
                    exit(1);
                }
                return st;
            }
        }
        if (time(NULL) > deadline) {
            fprintf(stderr, "FAIL timeout waiting for %s\n", expected ? expected : "child exit");
            exit(1);
        }
    }
}

static void expect_contains(const char *needle) {
    if (!strstr(out_buf, needle)) {
        fprintf(stderr, "FAIL output missing: %s\n", needle);
        exit(1);
    }
}
static void expect_absent(const char *needle) {
    if (strstr(out_buf, needle)) {
        fprintf(stderr, "FAIL output unexpectedly present: %s\n", needle);
        exit(1);
    }
}

static void run_scenario(int scenario) {
    out_len = 0; out_buf[0] = '\0';
    atomic_store(&reveal_open, 0);
    fixture_all_closed = (scenario == SC_NO_ENTER_COMPLETE);
    fixture_open_port = (scenario == SC_NO_ENTER_COMPLETE) ? 0 : 80;
    memset(handles, 0, sizeof handles); active_handles = 0;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    int slave = open(ptsname(master), O_RDWR);
    assert(slave >= 0);
    struct termios before = {0}, after = {0};
    assert(tcgetattr(slave, &before) == 0);

    int sync_p[2], ctrl_p[2];
    assert(pipe(sync_p) == 0 && pipe(ctrl_p) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* child: the scanner process on the terminal */
        close(sync_p[0]); close(ctrl_p[1]);
        dup2(slave, STDIN_FILENO); dup2(slave, STDOUT_FILENO); dup2(slave, STDERR_FILENO);
        if (slave > 2) close(slave);
        close(master);
        char fdbuf[16]; snprintf(fdbuf, sizeof fdbuf, "%d", sync_p[1]);
        setenv("NERO_TEST_SYNC_FD", fdbuf, 1);
        unsetenv("NERO_SCAN_ICMP");
        ctrl_read_fd = ctrl_p[0];
        fcntl(ctrl_read_fd, F_SETFL, O_NONBLOCK);
        int rc = portscanner("127.0.0.1");
        fflush(NULL); /* _exit() below skips stdio flush; push every byte to the pty */
        int ok = scenario == SC_NO_ENTER_COMPLETE ? (rc == 0)
               : scenario == SC_ESC_CANCEL ? (rc == PORTSCANNER_CANCELLED)
               : (rc == fixture_open_port);
        _exit(ok ? 0 : 20);
    }

    /* parent: the person at the terminal */
    close(sync_p[1]); close(ctrl_p[0]);
    int sync_r = sync_p[0], ctrl_w = ctrl_p[1];
    fcntl(master, F_SETFL, O_NONBLOCK);
    fcntl(sync_r, F_SETFL, O_NONBLOCK);

    assert(write(master, "\n", 1) == 1);          /* accept the default speed profile */
    pump(master, sync_r, "SCAN-START", pid);

    if (scenario == SC_EARLY_SELECT) {
        assert(write(master, "\n", 1) == 1);      /* Enter before any open port */
        pump(master, sync_r, "ENTER-IGNORED", pid); /* must NOT start selection */
        assert(write(ctrl_w, "r", 1) == 1);       /* reveal the open port */
        pump(master, sync_r, "OPEN", pid);
        assert(write(master, "\n", 1) == 1);      /* Enter now stops the scan early */
        pump(master, sync_r, "ENTER-STOP", pid);
        assert(write(master, "\n", 1) == 1);      /* pick the highlighted open port */
        int st = pump(master, sync_r, NULL, pid);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0); /* rc == open port */
        expect_contains("status=stopped early by user (partial coverage)");
        expect_contains("were not scanned");
        expect_contains("\x1b[?1006l");           /* mouse reporting restored */
    } else if (scenario == SC_ESC_CANCEL) {
        assert(write(ctrl_w, "r", 1) == 1);       /* an open port exists mid-scan */
        pump(master, sync_r, "OPEN", pid);
        assert(write(master, "\x1b", 1) == 1);    /* Esc cancels the whole operation */
        int st = pump(master, sync_r, NULL, pid);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0); /* rc == PORTSCANNER_CANCELLED */
        expect_contains("Scan cancelled");
        expect_absent("stopped early by user");
        expect_contains("\x1b[?1006l");
    } else { /* SC_NO_ENTER_COMPLETE */
        int st = pump(master, sync_r, NULL, pid);  /* no Enter: scan runs to completion */
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        expect_contains("status=complete");
        expect_contains("unscanned=0");
        expect_absent("stopped early by user");
    }

    assert(tcgetattr(slave, &after) == 0);
#if !defined(PENDIN) && defined(__APPLE__)
#define PENDIN 0x20000000
#endif
#ifdef PENDIN
    before.c_lflag &= ~(tcflag_t)PENDIN;
    after.c_lflag  &= ~(tcflag_t)PENDIN;
#endif
    if (memcmp(&before, &after, sizeof before) != 0) {
        fprintf(stderr, "FAIL slave termios not restored after scenario %d\n", scenario);
        exit(1);
    }
    close(master); close(slave); close(sync_r); close(ctrl_w);
}

int main(void) {
    run_scenario(SC_EARLY_SELECT);
    puts("PASS pty: Enter after first open port stops the scan and opens the port menu (partial)");
    run_scenario(SC_ESC_CANCEL);
    puts("PASS pty: Esc cancels the whole operation, distinct from early selection");
    run_scenario(SC_NO_ENTER_COMPLETE);
    puts("PASS pty: without Enter the scan completes normally");
    puts("PASS live-select pty: deterministic, mock sockets only, terminal restored on every path");
    return 0;
}
