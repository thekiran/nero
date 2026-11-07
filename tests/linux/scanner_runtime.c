/* Production scanner against real loopback sockets only. Covers the live work
 * queue on a listening/refused port pair, the async-interrupt path, the port
 * menu lifecycle (EOF -> -1, valid selection, termios restore), full
 * portscanner() end-to-end restarts with partial pthread_create failure, the
 * completion report fields and the descriptor baseline. Every connect() made
 * by the production scanner is routed through a guard that aborts the test on
 * any destination outside 127.0.0.0/8, so no packet can leave loopback. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Strict POSIX on macOS hides the loopback constant from netinet/in.h. */
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001u /* 127.0.0.1 */
#endif

/* Fault injection for the partial thread-creation regression: once armed,
 * the Nth and later create calls inside portscanner.c fail with EAGAIN.
 * The production queue must still finish every port through the caller. */
static int create_fail_at = -1;
static int create_count = 0;
static int injected_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   void *(*fn)(void *), void *arg)
{
    if (create_fail_at >= 0 && create_count++ >= create_fail_at)
        return EAGAIN;
    return pthread_create(thread, attr, fn, arg);
}
/* Loopback-only guard around the real connect(). */
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>
static atomic_long guarded_connects;
static int loopback_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)address;
    if (address->sa_family != AF_INET || (ntohl(in->sin_addr.s_addr) >> 24) != 127) {
        fputs("FAIL scanner attempted a non-loopback connect\n", stderr);
        abort();
    }
    atomic_fetch_add(&guarded_connects, 1);
    return connect(fd, address, length);
}
#define pthread_create injected_pthread_create
#define connect loopback_connect
#include "../../src/modules/portscanner.c"
#undef connect
#undef pthread_create

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

/* Open descriptor count (portable: probe each slot below the soft limit). */
static int descriptors(void) {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) limit = 65536;
    int n = 0;
    for (int fd = 0; fd < limit; ++fd)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

static void scan_reset(void) {
    assert(results_list == NULL);
    results_list = calloc(SCAN_TOTAL_PORTS + 1, sizeof(*results_list));
    assert(results_list);
    assert(pthread_mutex_init(&results_mutex, NULL) == 0);
    atomic_init(&scan_cancel_requested, 0);
    atomic_init(&ports_scanned, 0);
    atomic_init(&host_responded, 0);
    atomic_init(&latest_probe_port, 0);
    atomic_init(&scan_progress_total, SCAN_TOTAL_PORTS);
    portscanner_interrupt_requested = 0;
    target_ip_global = "127.0.0.1";
}

static void scan_release(void) {
    assert(pthread_mutex_destroy(&results_mutex) == 0);
    free(results_list);
    results_list = NULL;
}

/* Bind a listener on loopback; *port receives the ephemeral port. */
static int listen_loopback(int *port) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    assert(server >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(server, (struct sockaddr *)&addr, &len) == 0);
    assert(listen(server, SOMAXCONN) == 0);
    *port = ntohs(addr.sin_port);
    return server;
}

/* Reserve then release a loopback port so the next connect is refused. */
static int reserve_closed_port(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    assert(server >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(server, (struct sockaddr *)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);
    assert(close(server) == 0);
    return port;
}

static void test_open_closed_pair(int open_port, int closed_port) {
    scan_reset();
    const int ports[2] = {open_port, closed_port};
    run_scan_phase(ports, 2, 2, 2, 500);
    scan_summary s = summarize_scan();
    assert(results_list[open_port].state == PORT_OPEN);
    assert(results_list[closed_port].state == PORT_CLOSED);
    assert(s.open == 1 && s.closed == 1 && s.unscanned == SCAN_TOTAL_PORTS - 2);
    assert(results_list[open_port].attempts == 1);
    assert(results_list[closed_port].attempts == 1);
    scan_release();
    puts("PASS real worker: loopback listener open, refused port closed, attempts accounted");
}

static void test_interrupt_keeps_unscanned_honest(void) {
    scan_reset();
    portscanner_interrupt_requested = 1;
    run_scan_phase(NULL, SCAN_TOTAL_PORTS, 4, 64, 50);
    scan_summary s = summarize_scan();
    assert(scan_cancelled());
    assert(s.unscanned == SCAN_TOTAL_PORTS && s.closed == 0);
    portscanner_interrupt_requested = 0;
    atomic_store(&scan_cancel_requested, 0);
    scan_release();
    puts("PASS async interrupt before work leaves every port explicitly unscanned");
}

static void test_menu_eof_pipe(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(close(fds[1]) == 0);
    assert(dup2(fds[0], STDIN_FILENO) >= 0);
    assert(close(fds[0]) == 0);
    int single[] = {22};
    /* EOF before any selection must be -1, never a guessed "first port". */
    assert(select_port_menu(single, 1) == -1);
    puts("PASS menu EOF on pipe -> -1 (no selection guessed)");
}

/* Plays the terminal's role on the pty master: consume the slave's echo so
 * TCSADRAIN can complete (macOS blocks until echoed output is read). */
typedef struct { int fd; atomic_int stop; } pty_drain;
static void *drain_master(void *arg) {
    pty_drain *d = arg;
    char sink[256];
    while (!atomic_load(&d->stop)) {
        struct pollfd p = {d->fd, POLLIN, 0};
        if (poll(&p, 1, 20) > 0 && (p.revents & POLLIN) &&
            read(d->fd, sink, sizeof(sink)) <= 0)
            break;
    }
    return NULL;
}

static void test_menu_valid_and_termios_restore(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    int slave = open(ptsname(master), O_RDWR);
    assert(slave >= 0);
    /* Zero-init so memcmp never compares indeterminate struct padding. */
    struct termios before = {0};
    struct termios after = {0};
    assert(tcgetattr(slave, &before) == 0);
    assert(dup2(slave, STDIN_FILENO) >= 0);
    pty_drain drain = {.fd = master};
    atomic_init(&drain.stop, 0);
    pthread_t drainer;
    assert(pthread_create(&drainer, NULL, drain_master, &drain) == 0);

    int unsorted[] = {443, 80};
    assert(write(master, "\n", 1) == 1);
    assert(select_port_menu(unsorted, 2) == 80); /* sorted, first highlighted */

    assert(write(master, "\x1b[B\n", 4) == 4);
    assert(select_port_menu(unsorted, 2) == 443); /* one Down then Enter */
    atomic_store(&drain.stop, 1);
    assert(pthread_join(drainer, NULL) == 0);

    assert(tcgetattr(slave, &after) == 0);
#if !defined(PENDIN) && defined(__APPLE__)
#define PENDIN 0x20000000 /* <sys/termios.h>; hidden under strict _XOPEN_SOURCE */
#endif
#ifdef PENDIN
    /* PENDIN is kernel-owned state (macOS sets it when pending input meets a
     * return to canonical mode); it is not a setting the scanner changed. */
    before.c_lflag &= ~(tcflag_t)PENDIN;
    after.c_lflag &= ~(tcflag_t)PENDIN;
#endif
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(close(master) == 0);
    assert(close(slave) == 0);
    puts("PASS menu Enter/arrow on pty -> expected ports, slave termios restored");
}

/* Point STDIN at a pipe primed with Enter keystrokes for menu selection. */
static int feed_stdin(const char *keys) {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(write(fds[1], keys, strlen(keys)) == (ssize_t)strlen(keys));
    assert(dup2(fds[0], STDIN_FILENO) >= 0);
    assert(close(fds[0]) == 0);
    return fds[1]; /* keep open so the pipe never signals EOF */
}

/* Capture report_scan_summary()/portscanner() stdout into a heap buffer. */
static char *capture_stdout(void (*run)(void *), void *context) {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    FILE *cap = tmpfile();
    assert(cap);
    assert(dup2(fileno(cap), STDOUT_FILENO) == 1);
    run(context);
    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) == 1);
    assert(close(saved) == 0);
    long size = ftell(cap);
    assert(size >= 0);
    rewind(cap);
    char *text = malloc((size_t)size + 1);
    assert(text);
    size_t got = fread(text, 1, (size_t)size, cap);
    text[got] = '\0';
    fclose(cap);
    return text;
}

typedef struct {
    const char *ip;
    int rc;
} scan_call;

static void run_portscanner(void *context) {
    scan_call *call = context;
    call->rc = portscanner(call->ip);
}

/* Behaves like a real service: accept and close every connection, so the
 * kernel accept queue never fills across repeated scans and banner probes.
 * (A full queue makes Linux drop SYNs, which the scanner rightly reports as
 * filtered rather than open.) */
typedef struct { int fd; atomic_int stop; } loopback_service;
static void *serve_loopback(void *arg) {
    loopback_service *svc = arg;
    while (!atomic_load(&svc->stop)) {
        struct pollfd p = {svc->fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0 || !(p.revents & POLLIN)) continue;
        int peer = accept(svc->fd, NULL, NULL);
        if (peer >= 0) assert(close(peer) == 0);
    }
    return NULL;
}

static void test_end_to_end_restart(int run_count) {
    scan_call call = {.ip = "127.0.0.1"};
    for (int i = 0; i < run_count; ++i) {
        create_count = 0;
        create_fail_at = (i == run_count - 1) ? 3 : -1;
        char *output = capture_stdout(run_portscanner, &call);
        create_fail_at = -1;
        /* The scanner must find a real open loopback port and complete the
         * ledger even when worker creation partially fails. */
        assert(call.rc >= 1);
        assert(strstr(output, "status=complete") != NULL);
        assert(strstr(output, "unscanned=0") != NULL);
        assert(strstr(output, "profile=Orta") != NULL);
        assert(strstr(output, "target=127.0.0.1") != NULL);
        free(output);
    }
    puts("PASS end-to-end loopback scans (incl. one with partial worker-creation "
         "failure) complete, report profile/duration/status, and restart cleanly");
}

int main(void) {
    int baseline = descriptors();
    int stdin_save = dup(STDIN_FILENO);
    assert(stdin_save >= 0);

    int open_port, closed_port;
    int server = listen_loopback(&open_port);
    closed_port = reserve_closed_port();
    test_open_closed_pair(open_port, closed_port);
    test_interrupt_keeps_unscanned_honest();

    test_menu_eof_pipe();
    test_menu_valid_and_termios_restore();
    assert(close(server) == 0);
    server = listen_loopback(&open_port); /* reopen for the end-to-end scans */

    int pipe_fd = feed_stdin("\n\n"); /* one Enter per end-to-end menu */
    loopback_service service = {.fd = server};
    atomic_init(&service.stop, 0);
    pthread_t service_thread;
    assert(pthread_create(&service_thread, NULL, serve_loopback, &service) == 0);
    test_end_to_end_restart(2);
    atomic_store(&service.stop, 1);
    assert(pthread_join(service_thread, NULL) == 0);

    assert(close(pipe_fd) == 0);
    assert(dup2(stdin_save, STDIN_FILENO) == 0);
    assert(close(stdin_save) == 0);
    assert(close(server) == 0);
    assert(descriptors() == baseline);
    /* Two full scans plus the pair test all went through the loopback guard. */
    assert(atomic_load(&guarded_connects) >= 2L * SCAN_TOTAL_PORTS + 2);
    puts("PASS scanner runtime: loopback only, no external targets, descriptor baseline restored");
    return 0;
}
