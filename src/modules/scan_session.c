/* Production TCP scan core: no terminal dependencies. */
#include "scan_internal.h"

#include "nero/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
const scan_profile SCAN_PROFILES[] = {
    /* Fast, balanced and slow timeout/batch preferences. The per-session socket
     * budget is enforced independently; all profiles retry every uncertainty. */
    {"Hizli", 8, 256, 500, 4, 128, 1000, 8000, 1},
    /* Orta (önerilen): dengeli. */
    {"Orta", 6, 200, 1000, 4, 100, 2000, 12000, 2},
    /* Yavaş: düşük eşzamanlılık, uzun zaman aşımı. En kararlı/eksiksiz. */
    {"Yavas", 4, 100, 1500, 2, 64, 3000, 65535, 3},
};

int scan_cancelled(nero_scan_session *s)
{
    if (s->check_cancel && s->check_cancel(s->cancel_context))
        atomic_store(&s->scan_cancel_requested, 1);
    return atomic_load(&s->fatal_error) || (s->interrupt_requested && *s->interrupt_requested) ||
           atomic_load(&s->scan_cancel_requested);
}

int scan_interrupted(nero_scan_session *s)
{
    return scan_cancelled(s) ||
           atomic_load_explicit(&s->scan_select_requested, memory_order_relaxed);
}

int64_t scan_now_ms(nero_scan_session *s)
{
    struct timespec now;
    if (s->io.clock_fn(CLOCK_MONOTONIC, &now) != 0) {
        int error = errno ? errno : EIO, expected = 0;
        if (atomic_compare_exchange_strong(&s->fatal_error, &expected, error))
            nero_log_write(NERO_LOG_ERROR, "scanner", "Monotonic clock failed: error=%d", error);
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

scan_summary summarize_scan(nero_scan_session *s)
{
    scan_summary counts = {0};
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port) {
        switch (s->results_list[port].state) {
        case PORT_OPEN:
            counts.open++;
            break;
        case PORT_CLOSED:
            counts.closed++;
            break;
        case PORT_FILTERED:
            counts.filtered++;
            break;
        case PORT_UNKNOWN:
            counts.unknown++;
            break;
        case PORT_UNSCANNED:
            counts.unscanned++;
            break;
        }
        if (s->results_list[port].retries_completed)
            counts.retried++;
        counts.retry_probes += (int)s->results_list[port].retries_completed;
    }
    return counts;
}

static int state_evidence(port_state state)
{
    switch (state) {
    case PORT_OPEN:
        return 3;
    case PORT_CLOSED:
        return 2;
    case PORT_FILTERED:
        return 1;
    case PORT_UNKNOWN:
    case PORT_UNSCANNED:
        break;
    }
    return 0;
}

void record_result(nero_scan_session *s, int port, port_state state, int error,
                   scan_error_stage stage)
{
    pthread_mutex_lock(&s->results_mutex);
    port_result *r = &s->results_list[port];
    if (r->state == PORT_UNSCANNED)
        atomic_fetch_add_explicit(&s->ports_scanned, 1, memory_order_relaxed);
    else
        r->retries_completed++;
    if (r->state == PORT_UNSCANNED || state_evidence(state) >= state_evidence(r->state)) {
        r->state = state;
        r->error = error;
        r->error_stage = stage;
    }
    pthread_mutex_unlock(&s->results_mutex);
    atomic_fetch_add_explicit(&s->probes_completed, 1, memory_order_relaxed);
    if (state == PORT_OPEN || state == PORT_CLOSED)
        atomic_store_explicit(&s->host_responded, 1, memory_order_relaxed);
    if (state == PORT_OPEN && s->on_open)
        s->on_open(s->event_context);
}

int scan_progress_permille(nero_scan_session *s)
{
    long done = atomic_load_explicit(&s->probes_completed, memory_order_relaxed);
    long total = atomic_load_explicit(&s->scan_progress_total, memory_order_relaxed);
    if (total < SCAN_TOTAL_PORTS)
        total = SCAN_TOTAL_PORTS;
    int permille = (int)(done * 1000 / total);
    if (atomic_load_explicit(&s->scan_work_done, memory_order_relaxed))
        return 1000;
    return permille > 999 ? 999 : permille;
}

int summarize_scan_errors(nero_scan_session *s, scan_error_group *groups, int *overflow)
{
    int count = 0;
    *overflow = 0;
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port) {
        const port_result *r = &s->results_list[port];
        if (r->state != PORT_UNKNOWN)
            continue;
        int i;
        for (i = 0; i < count; ++i)
            if (groups[i].stage == r->error_stage && groups[i].error == r->error)
                break;
        if (i == count) {
            if (count == SCAN_ERROR_GROUPS) {
                (*overflow)++;
                continue;
            }
            groups[count++] = (scan_error_group){r->error_stage, r->error, 0};
        }
        groups[i].count++;
    }
    return count;
}

int scan_socket_budget(void)
{
    int budget = MAX_SCAN_SOCKETS;
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        rlim_t available = rl.rlim_cur > 64 ? rl.rlim_cur - 64 : 1;
        if (available < (rlim_t)budget)
            budget = (int)available;
    }
#endif
    return budget;
}

static port_state classify_connect_error(int error)
{
    if (error == 0)
        return PORT_OPEN;
    if (error == ECONNREFUSED)
        return PORT_CLOSED;
    if (error == ETIMEDOUT)
        return PORT_FILTERED;
    return PORT_UNKNOWN;
}

static int is_resource_error(int error)
{
    return error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM ||
           error == EADDRNOTAVAIL;
}

int connect_pending(int error)
{
    return error == EINPROGRESS || error == EALREADY || error == EWOULDBLOCK || error == EINTR
#ifdef _WIN32
           || error == WSAEINTR || error == WSAEINPROGRESS
#endif
        ;
}

int make_nonblocking(nero_socket_t fd)
{
#ifdef _WIN32
    u_long value = 1;
    if (ioctlsocket(fd, FIONBIO, &value) == 0)
        return 0;
    errno = WSAGetLastError();
    return -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void scan_ports_batch(nero_scan_session *s, const char *ip, const int *ports, int count,
                      int timeout_ms)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
#if defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    if (count <= 0 || count > MAX_BATCH || timeout_ms <= 0 ||
        inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        return;

    struct pollfd pfds[MAX_BATCH];
    int slot_port[MAX_BATCH];
    int64_t deadline[MAX_BATCH];
    int nfds = 0, remaining = 0, resource_error = 0;
    for (int i = 0; i < count && !scan_interrupted(s); ++i) {
        int port = ports[i];
        /* Port 0 and out-of-range indices can never enter the ledger; the
         * production queue only generates the complete 1..65535 range. */
        if (port < 1 || port > SCAN_TOTAL_PORTS)
            continue;
        pthread_mutex_lock(&s->results_mutex);
        s->results_list[port].attempts++;
        pthread_mutex_unlock(&s->results_mutex);
        atomic_store_explicit(&s->latest_probe_port, port, memory_order_relaxed);
        nero_socket_t fd = s->io.socket_fn(AF_INET, SOCK_STREAM, 0);
        if (fd == NERO_INVALID_SOCKET) {
            int error = errno;
            record_result(s, port, PORT_UNKNOWN, error, SCAN_SOCKET);
            if (is_resource_error(error))
                resource_error = 1;
            continue;
        }
        if (s->io.nonblocking_fn(fd) != 0) {
            int error = errno;
            s->io.close_fn(fd);
            record_result(s, port, PORT_UNKNOWN, error, SCAN_NONBLOCKING);
            continue;
        }
        addr.sin_port = htons((uint16_t)port);
        deadline[nfds] = scan_now_ms(s) + timeout_ms;
        if (atomic_load(&s->fatal_error)) {
            s->io.close_fn(fd);
            break;
        }
        int ret = s->io.connect_fn(fd, (struct sockaddr *)&addr, sizeof(addr));
        int error = ret == 0 ? 0 : errno;
        if (ret == 0 || !connect_pending(error)) {
            s->io.close_fn(fd);
            record_result(s, port, classify_connect_error(error), error, SCAN_CONNECT);
            if (is_resource_error(error))
                resource_error = 1;
            continue;
        }
        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        slot_port[nfds++] = port;
        remaining++;
    }

    while (remaining && !scan_interrupted(s)) {
        int wait_ms = CANCEL_POLL_MS;
        int64_t now = scan_now_ms(s);
        for (int slot = 0; slot < nfds; ++slot) {
            if (pfds[slot].fd == NERO_INVALID_SOCKET)
                continue;
            int64_t left = deadline[slot] - now;
            if (left <= 0)
                wait_ms = 0;
            else if (left < wait_ms)
                wait_ms = (int)left;
        }
        for (int slot = 0; slot < nfds; ++slot)
            pfds[slot].revents = 0;
        int before = remaining;
        int ready = s->io.poll_fn(pfds, (nfds_t)nfds, wait_ms);
        if (scan_interrupted(s))
            break;
        if (ready < 0) {
            int error = errno;
            if (error == EINTR
#ifdef _WIN32
                || error == WSAEINTR
#endif
            ) {
                /* poll may leave revents unspecified on error. Still expire
                 * absolute deadlines even under a stream of interruptions. */
                ready = 0;
                for (int slot = 0; slot < nfds; ++slot)
                    pfds[slot].revents = 0;
            } else {
                for (int slot = 0; slot < nfds; ++slot) {
                    if (pfds[slot].fd == NERO_INVALID_SOCKET)
                        continue;
                    s->io.close_fn(pfds[slot].fd);
                    pfds[slot].fd = NERO_INVALID_SOCKET;
                    record_result(s, slot_port[slot], PORT_UNKNOWN, error, SCAN_POLL);
                }
                break;
            }
        }
        for (int slot = 0; slot < nfds; ++slot) {
            if (pfds[slot].fd == NERO_INVALID_SOCKET || !pfds[slot].revents)
                continue;
            int error = 0;
            socklen_t len = sizeof(error);
            port_state state;
            scan_error_stage stage = SCAN_SO_ERROR;
            if (pfds[slot].revents & POLLNVAL) {
                error = EBADF;
                stage = SCAN_POLL_EVENT;
                state = PORT_UNKNOWN;
            } else if (s->io.getsockopt_fn(pfds[slot].fd, SOL_SOCKET, SO_ERROR, &error, &len) !=
                       0) {
                error = errno;
                state = PORT_UNKNOWN;
            } else if (connect_pending(error)) {
                continue; /* readiness is not itself successful connection evidence */
                /* Writability with SO_ERROR=0 confirms the handshake even if
                 * the peer already closed. HUP alone is not proof of success. */
            } else if (!error &&
                       (!(pfds[slot].revents & POLLOUT) || (pfds[slot].revents & POLLERR))) {
                stage = SCAN_POLL_EVENT;
                state = PORT_UNKNOWN;
            } else
                state = classify_connect_error(error);
            s->io.close_fn(pfds[slot].fd);
            pfds[slot].fd = NERO_INVALID_SOCKET;
            record_result(s, slot_port[slot], state, error, stage);
            remaining--;
        }
        now = scan_now_ms(s);
        for (int slot = 0; slot < nfds; ++slot) {
            if (pfds[slot].fd == NERO_INVALID_SOCKET || deadline[slot] > now)
                continue;
            s->io.close_fn(pfds[slot].fd);
            pfds[slot].fd = NERO_INVALID_SOCKET;
            record_result(s, slot_port[slot], PORT_FILTERED, ETIMEDOUT, SCAN_DEADLINE);
            remaining--;
        }
        if (ready > 0 && remaining == before) {
            const struct timespec delay = {0, 1000000L};
            s->io.sleep_fn(&delay, NULL); /* spurious/pending readiness must not spin */
        }
    }
    /* Cancellation leaves unfinished slots unscanned, or retains their first
     * phase outcome. No cancelled probe is reported as a completed attempt. */
    for (int slot = 0; slot < nfds; ++slot)
        if (pfds[slot].fd != NERO_INVALID_SOCKET)
            s->io.close_fn(pfds[slot].fd);
    if (resource_error)
        atomic_fetch_add_explicit(&s->resource_failures, 1, memory_order_relaxed);
    if (resource_error && !scan_interrupted(s)) {
        const struct timespec delay = {0, 5000000L};
        s->io.sleep_fn(&delay, NULL); /* bounded backoff, not a busy socket retry loop */
    }
}

static void *scan_worker(void *arg)
{
    scan_work *work = arg;
    nero_scan_session *s = work->session;
    int ports[MAX_BATCH];
    while (!scan_interrupted(s)) {
        int base = atomic_fetch_add_explicit(&work->next, work->batch, memory_order_relaxed);
        if (base >= work->count)
            break;
        int count = work->count - base;
        if (count > work->batch)
            count = work->batch;
        for (int i = 0; i < count; ++i)
            ports[i] = work->ports ? work->ports[base + i] : base + i + 1;
        scan_ports_batch(s, s->target_ip_global, ports, count, work->timeout_ms);
    }
    return NULL;
}

void run_scan_phase(nero_scan_session *s, const int *ports, int count, int threads, int batch,
                    int timeout_ms)
{
    int budget = scan_socket_budget();
    if (threads < 1)
        threads = 1;
    if (threads > MAX_THREADS)
        threads = MAX_THREADS;
    if (threads > budget)
        threads = budget;
    if (batch < 1)
        batch = 1;
    if (batch > MAX_BATCH)
        batch = MAX_BATCH;
    if (batch > budget / threads)
        batch = budget / threads;
    scan_work work = {
        .session = s, .ports = ports, .count = count, .timeout_ms = timeout_ms, .batch = batch};
    atomic_init(&work.next, 0);
    pthread_t tids[MAX_THREADS];
    int created = 0;
    /* Caller is also a worker, so partial/all pthread_create failure cannot
     * lose a statically assigned range. Total scanner workers <= threads. */
    for (int i = 1; i < threads; ++i) {
        if (s->io.thread_create_fn(&tids[created], NULL, scan_worker, &work) != 0) {
            atomic_fetch_add(&s->worker_failures, 1);
            break;
        }
        created++;
    }
    scan_worker(&work);
    for (int i = 0; i < created; ++i)
        pthread_join(tids[i], NULL);
}

int collect_retry_candidates(nero_scan_session *s, int *ports)
{
    int count = 0;
    for (int port = 1; port <= SCAN_TOTAL_PORTS; ++port)
        if (s->results_list[port].state == PORT_FILTERED ||
            s->results_list[port].state == PORT_UNKNOWN)
            ports[count++] = port;
    return count;
}

void run_deep_scan(nero_scan_session *s, int *ports, int count, const scan_profile *profile)
{
    int window = profile->deep_max_candidates;
    if (window < 1 || window > SCAN_TOTAL_PORTS)
        window = SCAN_TOTAL_PORTS;
    int rounds = profile->deep_rounds;
    if (rounds < 1)
        rounds = 1;
    if (rounds > MAX_DEEP_ROUNDS)
        rounds = MAX_DEEP_ROUNDS;
    int timeout = profile->deep_timeout_ms;
    if (timeout < profile->fast_timeout_ms)
        timeout = profile->fast_timeout_ms;
    if (timeout > MAX_DEEP_TIMEOUT_MS)
        timeout = MAX_DEEP_TIMEOUT_MS;
    int threads = profile->deep_threads;
    if (threads < 1)
        threads = 1;
    if (threads > MAX_THREADS)
        threads = MAX_THREADS;
    int batch = profile->deep_batch;
    if (batch < 1)
        batch = 1;
    if (batch > (MAX_SCAN_SOCKETS / 2) / threads)
        batch = (MAX_SCAN_SOCKETS / 2) / threads;

    for (int round = 0; round < rounds && count > 0 && !scan_interrupted(s); ++round) {
        /* Publish this round's work before probing so progress stays honest. */
        atomic_fetch_add_explicit(&s->scan_progress_total, count, memory_order_relaxed);
        for (int start = 0; start < count && !scan_interrupted(s); start += window) {
            int chunk = count - start;
            if (chunk > window)
                chunk = window;
            int before = atomic_load_explicit(&s->resource_failures, memory_order_relaxed);
            run_scan_phase(s, ports + start, chunk, threads, batch, timeout);
            if (atomic_load_explicit(&s->resource_failures, memory_order_relaxed) != before) {
                if (threads > 1)
                    threads /= 2;
                else if (batch > 1)
                    batch /= 2;
            }
        }
        /* Every writer of this round has been joined by run_scan_phase. */
        int next = 0;
        for (int i = 0; i < count; ++i)
            if (s->results_list[ports[i]].state == PORT_UNKNOWN)
                ports[next++] = ports[i];
        count = next;
    }
}

static int valid_profile(const scan_profile *p)
{
    return p->fast_threads >= 1 && p->fast_threads <= MAX_THREADS && p->deep_threads >= 1 &&
           p->deep_threads <= MAX_THREADS && p->fast_batch >= 1 && p->fast_batch <= MAX_BATCH &&
           p->deep_batch >= 1 && p->deep_batch <= MAX_BATCH && p->fast_timeout_ms >= 1 &&
           p->fast_timeout_ms <= MAX_DEEP_TIMEOUT_MS && p->deep_timeout_ms >= 1 &&
           p->deep_timeout_ms <= MAX_DEEP_TIMEOUT_MS && p->deep_max_candidates >= 1 &&
           p->deep_max_candidates <= SCAN_TOTAL_PORTS && p->deep_rounds >= 1 &&
           p->deep_rounds <= MAX_DEEP_ROUNDS;
}
nero_scan_session *scan_create_with_io(const char *target, const scan_profile *profile,
                                       const scan_io *io)
{
    struct in_addr address;
    const scan_profile *p = profile ? profile : &SCAN_PROFILES[PROFILE_MEDIUM];
    if (!target || inet_pton(AF_INET, target, &address) != 1 || !valid_profile(p)) {
        errno = EINVAL;
        return NULL;
    }
    nero_scan_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->io =
        (scan_io){socket,           connect,       getsockopt, close,          nero_socket_poll,
                  make_nonblocking, clock_gettime, nanosleep,  pthread_create, pthread_mutex_init};
#define OVERRIDE(f)                                                                                \
    if (io && io->f)                                                                               \
    s->io.f = io->f
    OVERRIDE(socket_fn);
    OVERRIDE(connect_fn);
    OVERRIDE(getsockopt_fn);
    OVERRIDE(close_fn);
    OVERRIDE(poll_fn);
    OVERRIDE(nonblocking_fn);
    OVERRIDE(clock_fn);
    OVERRIDE(sleep_fn);
    OVERRIDE(thread_create_fn);
    OVERRIDE(mutex_init_fn);
#undef OVERRIDE
    memcpy(s->target, target, strlen(target) + 1);
    s->target_ip_global = s->target;
    s->profile = *p;
    snprintf(s->profile_name, sizeof s->profile_name, "%s", p->name ? p->name : "custom");
    s->profile.name = s->profile_name;
    s->scan_profile_label = s->profile.name;
    s->results_list = calloc(SCAN_TOTAL_PORTS + 1, sizeof(*s->results_list));
    s->work_ports = malloc(SCAN_TOTAL_PORTS * sizeof(*s->work_ports));
    int error = (!s->results_list || !s->work_ports) ? ENOMEM
                                                     : s->io.mutex_init_fn(&s->results_mutex, NULL);
    if (error) {
        nero_log_write(NERO_LOG_ERROR, "scanner",
                       "Scan mutex initialization failed or allocation failed: error=%d (%s)",
                       error, strerror(error));
        free(s->results_list);
        free(s->work_ports);
        free(s);
        errno = error;
        return NULL;
    }
    atomic_init(&s->scan_cancel_requested, 0);
    atomic_init(&s->ports_scanned, 0);
    atomic_init(&s->latest_probe_port, 0);
    atomic_init(&s->host_responded, 0);
    atomic_init(&s->scan_progress_total, SCAN_TOTAL_PORTS);
    atomic_init(&s->probes_completed, 0);
    atomic_init(&s->scan_work_done, 0);
    atomic_init(&s->scan_select_requested, 0);
    atomic_init(&s->resource_failures, 0);
    atomic_init(&s->worker_failures, 0);
    atomic_init(&s->fatal_error, 0);
    atomic_init(&s->lifecycle, 0);
    return s;
}
nero_scan_session *nero_scan_create(const char *target, const nero_scan_profile *profile)
{
    return scan_create_with_io(target, profile, NULL);
}
nero_scan_status nero_scan_run(nero_scan_session *s)
{
    if (!s) {
        errno = EINVAL;
        return NERO_SCAN_FAILED;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s->lifecycle, &expected, 1)) {
        errno = expected == 1 ? EBUSY : EALREADY;
        return NERO_SCAN_FAILED;
    }
    const scan_profile *p = &s->profile;
    run_scan_phase(s, NULL, SCAN_TOTAL_PORTS, p->fast_threads, p->fast_batch, p->fast_timeout_ms);
    int candidates = collect_retry_candidates(s, s->work_ports);
    s->retry_candidates = candidates;
    if (!scan_interrupted(s))
        run_deep_scan(s, s->work_ports, candidates, p);
    int cancelled = scan_cancelled(s), early = atomic_load(&s->scan_select_requested),
        fatal = atomic_load(&s->fatal_error);
    scan_summary counts = summarize_scan(s);
    nero_scan_status status = fatal       ? NERO_SCAN_FAILED
                              : cancelled ? NERO_SCAN_CANCELLED
                              : early     ? NERO_SCAN_PARTIAL
                              : (counts.unscanned || counts.retried < candidates)
                                  ? NERO_SCAN_FAILED
                                  : NERO_SCAN_COMPLETE;
    if (status == NERO_SCAN_COMPLETE)
        atomic_store(&s->scan_work_done, 1);
    atomic_store(&s->lifecycle, 2);
    if (fatal)
        errno = fatal;
    return status;
}
void nero_scan_cancel(nero_scan_session *s)
{
    if (s)
        atomic_store(&s->scan_cancel_requested, 1);
}
void nero_scan_stop_early(nero_scan_session *s)
{
    if (s)
        atomic_store(&s->scan_select_requested, 1);
}
int nero_scan_snapshot(nero_scan_session *s, nero_scan_counts *counts)
{
    if (!s || !counts) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&s->results_mutex);
    *counts = summarize_scan(s);
    pthread_mutex_unlock(&s->results_mutex);
    return 0;
}
int nero_scan_get_result(nero_scan_session *s, int port, nero_scan_result *result)
{
    if (!s || !result || port < 1 || port > SCAN_TOTAL_PORTS) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&s->results_mutex);
    const port_result *r = &s->results_list[port];
    *result = (nero_scan_result){r->state, r->error, r->attempts, r->retries_completed};
    pthread_mutex_unlock(&s->results_mutex);
    return 0;
}
void nero_scan_destroy(nero_scan_session *s)
{
    if (!s)
        return;
    pthread_mutex_destroy(&s->results_mutex);
    free(s->work_ports);
    free(s->results_list);
    free(s);
}
