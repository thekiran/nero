/* Normal link to the production core, with no UI/terminal object files.
 * All network callbacks are synthetic; no socket is opened. */
#include "../src/modules/scan_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static atomic_int handles, pending_seen;
static nero_socket_t new_socket(int domain, int type, int protocol)
{
    CHECK(domain == AF_INET && type == SOCK_STREAM && protocol == 0);
    atomic_fetch_add(&handles, 1);
    return (nero_socket_t)42;
}
static int close_socket(nero_socket_t fd)
{
    CHECK(fd == (nero_socket_t)42);
    CHECK(atomic_fetch_sub(&handles, 1) > 0);
    return 0;
}
static int nonblocking(nero_socket_t fd)
{
    (void)fd;
    return 0;
}
static int closed_connect(nero_socket_t fd, const struct sockaddr *a, socklen_t n)
{
    (void)fd;
    (void)n;
    CHECK(a->sa_family == AF_INET);
    errno = ECONNREFUSED;
    return -1;
}
static int edge_connect(nero_socket_t fd, const struct sockaddr *a, socklen_t n)
{
    (void)fd;
    (void)n;
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)a;
    CHECK(in->sin_family == AF_INET && ntohl(in->sin_addr.s_addr) == 0xC000020Bu);
    int port = ntohs(in->sin_port);
    if (port == 1 || port == 65535)
        return 0;
    errno = ECONNREFUSED;
    return -1;
}
static int pending_connect(nero_socket_t fd, const struct sockaddr *a, socklen_t n)
{
    (void)fd;
    (void)a;
    (void)n;
    errno = EINPROGRESS;
    return -1;
}
static int pending_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    CHECK(timeout >= 0 && timeout <= CANCEL_POLL_MS);
    for (nfds_t i = 0; i < n; ++i)
        fds[i].revents = 0;
    atomic_store(&pending_seen, 1);
    struct timespec delay = {0, (long)timeout * 1000000L};
    nanosleep(&delay, NULL);
    return 0;
}
static int fail_mutex(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)m;
    (void)a;
    return EAGAIN;
}
static int fail_thread(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p)
{
    (void)t;
    (void)a;
    (void)f;
    (void)p;
    return EAGAIN;
}
static int fail_clock(clockid_t c, struct timespec *t)
{
    (void)c;
    (void)t;
    errno = EIO;
    return -1;
}
static scan_io closed_io(void)
{
    return (scan_io){.socket_fn = new_socket,
                     .connect_fn = closed_connect,
                     .close_fn = close_socket,
                     .nonblocking_fn = nonblocking};
}
typedef struct {
    nero_scan_session *s;
    nero_scan_status status;
} call;
static void *run(void *p)
{
    call *c = p;
    c->status = nero_scan_run(c->s);
    return NULL;
}
static int total(nero_scan_counts c)
{
    return c.open + c.closed + c.filtered + c.unknown + c.unscanned;
}
static void lifecycle(void)
{
    CHECK(nero_scan_create(NULL, NULL) == NULL && errno == EINVAL);
    CHECK(nero_scan_create("192.0.2.999", NULL) == NULL && errno == EINVAL);
    CHECK(nero_scan_run(NULL) == NERO_SCAN_FAILED && errno == EINVAL);
    nero_scan_profile p = SCAN_PROFILES[PROFILE_MEDIUM];
    p.fast_threads = 0;
    CHECK(nero_scan_create("192.0.2.10", &p) == NULL && errno == EINVAL);
    scan_io io = closed_io();
    io.mutex_init_fn = fail_mutex;
    CHECK(scan_create_with_io("192.0.2.10", NULL, &io) == NULL && errno == EAGAIN);
    io = closed_io();
    io.thread_create_fn = fail_thread;
    char target[] = "192.0.2.10", label[] = "owned";
    p = SCAN_PROFILES[PROFILE_MEDIUM];
    p.name = label;
    nero_scan_session *s = scan_create_with_io(target, &p, &io);
    CHECK(s);
    memset(target, 'x', strlen(target));
    memset(label, 'x', strlen(label));
    CHECK(strcmp(s->target, "192.0.2.10") == 0 && strcmp(s->profile.name, "owned") == 0);
    CHECK(nero_scan_run(s) == NERO_SCAN_COMPLETE && atomic_load(&s->worker_failures) == 1);
    nero_scan_counts c;
    CHECK(nero_scan_snapshot(s, &c) == 0 && c.closed == 65535 && total(c) == 65535);
    CHECK(nero_scan_run(s) == NERO_SCAN_FAILED && errno == EALREADY);
    nero_scan_result result;
    CHECK(nero_scan_get_result(s, 0, &result) == -1 && errno == EINVAL);
    CHECK(nero_scan_get_result(s, 65536, &result) == -1 && errno == EINVAL);
    CHECK(nero_scan_get_result(s, 1, &result) == 0 && result.state == NERO_PORT_CLOSED &&
          result.attempts == 1);
    CHECK(nero_scan_get_result(s, 65535, &result) == 0 && result.state == NERO_PORT_CLOSED);
    nero_scan_destroy(s);
    nero_scan_destroy(NULL);
    io = closed_io();
    io.clock_fn = fail_clock;
    s = scan_create_with_io("192.0.2.10", NULL, &io);
    CHECK(s);
    CHECK(nero_scan_run(s) == NERO_SCAN_FAILED && errno == EIO);
    CHECK(nero_scan_snapshot(s, &c) == 0 && c.unscanned == 65535);
    CHECK(atomic_load(&handles) == 0);
    nero_scan_destroy(s);
    puts("PASS session ownership, validation, initialization/clock failures, caller fallback and "
         "one-shot lifecycle");
}
static void isolation(void)
{
    scan_io a = closed_io(), b = closed_io();
    b.connect_fn = edge_connect;
    call c[2] = {{scan_create_with_io("192.0.2.10", NULL, &a), NERO_SCAN_FAILED},
                 {scan_create_with_io("192.0.2.11", NULL, &b), NERO_SCAN_FAILED}};
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) {
        CHECK(c[i].s);
        CHECK(pthread_create(&threads[i], NULL, run, &c[i]) == 0);
    }
    for (int i = 0; i < 2; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0 && c[i].status == NERO_SCAN_COMPLETE);
        nero_scan_counts counts;
        CHECK(nero_scan_snapshot(c[i].s, &counts) == 0);
        CHECK(total(counts) == 65535 && counts.unscanned == 0 && counts.unknown == 0);
        CHECK(counts.open == (i ? 2 : 0) && counts.closed == 65535 - counts.open);
        nero_scan_destroy(c[i].s);
    }
    CHECK(atomic_load(&handles) == 0);
    puts("PASS simultaneous sessions keep targets, cancellation, ledgers and outcomes separate");
}
static void cancellation_isolation(void)
{
    scan_io a = closed_io(), b = closed_io();
    b.connect_fn = edge_connect;
    call calls[2] = {{scan_create_with_io("192.0.2.10", NULL, &a), NERO_SCAN_FAILED},
                     {scan_create_with_io("192.0.2.11", NULL, &b), NERO_SCAN_FAILED}};
    CHECK(calls[0].s && calls[1].s);
    nero_scan_cancel(calls[0].s);
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_create(&threads[i], NULL, run, &calls[i]) == 0);
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(calls[0].status == NERO_SCAN_CANCELLED && calls[1].status == NERO_SCAN_COMPLETE);
    nero_scan_counts counts;
    CHECK(nero_scan_snapshot(calls[0].s, &counts) == 0 && counts.unscanned == 65535);
    CHECK(nero_scan_snapshot(calls[1].s, &counts) == 0 && counts.open == 2 &&
          counts.closed == 65533);
    nero_scan_destroy(calls[0].s);
    nero_scan_destroy(calls[1].s);
    CHECK(atomic_load(&handles) == 0);
    puts("PASS cancelling one session cannot stop a concurrent independent session");
}

static void stops(void)
{
    for (int early = 0; early < 2; ++early) {
        scan_io io = closed_io();
        io.connect_fn = pending_connect;
        io.poll_fn = pending_poll;
        call c = {scan_create_with_io("192.0.2.10", NULL, &io), NERO_SCAN_FAILED};
        CHECK(c.s);
        atomic_store(&pending_seen, 0);
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, run, &c) == 0);
        int64_t deadline = scan_now_ms(c.s) + 5000;
        while (!atomic_load(&pending_seen)) {
            CHECK(scan_now_ms(c.s) < deadline);
            struct timespec d = {0, 1000000L};
            nanosleep(&d, NULL);
        }
        CHECK(nero_scan_run(c.s) == NERO_SCAN_FAILED && errno == EBUSY);
        nero_scan_counts counts;
        CHECK(nero_scan_snapshot(c.s, &counts) == 0 && total(counts) == 65535);
        nero_scan_result result;
        CHECK(nero_scan_get_result(c.s, 1, &result) == 0);
        int64_t start = scan_now_ms(c.s);
        if (early)
            nero_scan_stop_early(c.s);
        else
            nero_scan_cancel(c.s);
        CHECK(pthread_join(thread, NULL) == 0);
        /* 50ms poll slices, with scheduler allowance; not a real-time promise. */
        CHECK(scan_now_ms(c.s) - start < 1000);
        CHECK(c.status == (early ? NERO_SCAN_PARTIAL : NERO_SCAN_CANCELLED));
        CHECK(nero_scan_snapshot(c.s, &counts) == 0 && counts.unscanned == 65535);
        CHECK(atomic_load(&handles) == 0);
        nero_scan_destroy(c.s);
        io = closed_io();
        nero_scan_session *next = scan_create_with_io("192.0.2.10", NULL, &io);
        CHECK(next);
        CHECK(nero_scan_run(next) == NERO_SCAN_COMPLETE);
        nero_scan_destroy(next);
    }
    puts("PASS concurrent snapshots, busy rejection, bounded stop, socket cleanup and fresh "
         "restart");
}
int main(void)
{
    lifecycle();
    isolation();
    cancellation_isolation();
    stops();
    CHECK(atomic_load(&handles) == 0);
    return 0;
}
