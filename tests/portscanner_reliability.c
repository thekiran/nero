/* No network I/O: compile the production scanner with syscall substitutions.
 * Real pthreads exercise its queue; all socket handles/results are synthetic. */
#ifdef _WIN32
#include "../src/platform/windows/nero_win_compat.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <poll.h>
#include <sys/resource.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nero/socket_handle.h"

static nero_socket_t fake_socket(int, int, int);
static int fake_connect(nero_socket_t, const struct sockaddr *, socklen_t);
static int fake_getsockopt(nero_socket_t, int, int, void *, socklen_t *);
static int fake_close(nero_socket_t);
static int fake_poll(struct pollfd *, nfds_t, int);
static int fake_clock(clockid_t, struct timespec *);
static int fake_sleep(const struct timespec *, struct timespec *);
static int fake_thread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
#ifndef _WIN32
static int fake_fcntl(int, int, ...);
#else
static int fake_ioctl(SOCKET, long, u_long *);
#endif
/* Retain a non-macro path to the real pthread implementation. */
static int real_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *p) {
    return pthread_create(t, a, fn, p);
}
#undef socket
#undef connect
#undef getsockopt
#undef close
#undef nero_socket_poll
#define socket fake_socket
#define connect fake_connect
#define getsockopt fake_getsockopt
#define close fake_close
#define nero_socket_poll fake_poll
#define clock_gettime fake_clock
#define nanosleep fake_sleep
#define pthread_create fake_thread_create
#ifndef _WIN32
#define fcntl fake_fcntl
#else
#define ioctlsocket fake_ioctl
#endif
#include "../src/modules/portscanner.c"
#undef pthread_create

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define FAKE_HANDLES 1024
/* Include >INT_MAX handles on Windows to detect narrowing in scanner slots. */
#ifdef _WIN32
#define HANDLE_BASE ((nero_socket_t)1 << 40)
#else
#define HANDLE_BASE ((nero_socket_t)5000)
#endif
static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used, port, polls; } handles[FAKE_HANDLES];
static int attempts_seen[SCAN_TOTAL_PORTS + 1];
static atomic_llong fake_time;
static int scenario, failures, active_handles, peak_handles, thread_failure_after;
static atomic_int thread_creates;
enum { CLOSED, TIMEOUT, DELAYED, RESOURCE, MIXED, CANCEL, POLL_FAILURE,
       GETSOCKOPT_FAILURE, INVALID_EVENT, PENDING_EVENT, NONBLOCK_FAILURE, ROUTE_FAILURE,
       INTERRUPTED_POLL, ERROR_EVENT, TRANSIENT_RESOURCE, TRANSIENT_POLL };

static int handle_index(nero_socket_t fd) {
    CHECK(fd >= HANDLE_BASE && fd < HANDLE_BASE + FAKE_HANDLES);
    return (int)(fd - HANDLE_BASE);
}
static nero_socket_t fake_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    if (scenario == RESOURCE || (scenario == TRANSIENT_RESOURCE && failures++ < 20)) {
        errno=EMFILE; return NERO_INVALID_SOCKET;
    }
    pthread_mutex_lock(&fake_mutex);
    for (int i=0;i<FAKE_HANDLES;++i) if (!handles[i].used) {
        handles[i].used=1; handles[i].port=0; handles[i].polls=0;
        active_handles++;
        if (active_handles>peak_handles) peak_handles=active_handles;
        pthread_mutex_unlock(&fake_mutex);
        return HANDLE_BASE + (nero_socket_t)i;
    }
    pthread_mutex_unlock(&fake_mutex);
    CHECK(0); return NERO_INVALID_SOCKET;
}
static int fake_close(nero_socket_t fd) {
    pthread_mutex_lock(&fake_mutex);
    int i=handle_index(fd);
    CHECK(handles[i].used);
    handles[i].used=0; active_handles--;
    pthread_mutex_unlock(&fake_mutex);
    return 0;
}
static int fake_connect(nero_socket_t fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    int port=ntohs(((const struct sockaddr_in *)addr)->sin_port);
    CHECK(port>=1 && port<=SCAN_TOTAL_PORTS);
    pthread_mutex_lock(&fake_mutex);
    handles[handle_index(fd)].port=port;
    attempts_seen[port]++;
    pthread_mutex_unlock(&fake_mutex);
    if (scenario==CLOSED) { errno=ECONNREFUSED; return -1; }
    if (scenario==ROUTE_FAILURE) { errno=EHOSTUNREACH; return -1; }
    if (scenario==MIXED && port==54321) return 0; /* outside old discovery set */
    errno=EINPROGRESS; return -1;
}
static int fake_getsockopt(nero_socket_t fd, int level, int option, void *value, socklen_t *length) {
    (void)level; (void)option; (void)length;
    if (scenario==GETSOCKOPT_FAILURE) { errno=EIO; return -1; }
    pthread_mutex_lock(&fake_mutex);
    int i=handle_index(fd);
    int error=ECONNREFUSED;
    if (scenario==DELAYED || scenario==INTERRUPTED_POLL || scenario==TRANSIENT_POLL) error=0;
    if (scenario==ERROR_EVENT) error=0;
    if (scenario==PENDING_EVENT) error=handles[i].polls<3 ? EINPROGRESS : 0;
    *(int *)value=error;
    pthread_mutex_unlock(&fake_mutex);
    return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout) {
    CHECK(timeout>=0 && timeout<=CANCEL_POLL_MS);
    if (scenario==CANCEL) atomic_store(&scan_cancel_requested,1);
    if (scenario==POLL_FAILURE || (scenario==TRANSIENT_POLL && failures++==0)) { errno=EIO; return -1; }
    if (scenario==INTERRUPTED_POLL && failures++<3) { errno=EINTR; return -1; }
    int ready=0;
    pthread_mutex_lock(&fake_mutex);
    for (nfds_t s=0;s<count;++s) {
        fds[s].revents=0;
        if (fds[s].fd==NERO_INVALID_SOCKET) continue;
        int i=handle_index(fds[s].fd);
        handles[i].polls++;
        if (scenario==TIMEOUT || scenario==CANCEL || (scenario==MIXED && handles[i].port!=54321)) continue;
        if (scenario==DELAYED && handles[i].polls<3) continue;
        fds[s].revents=scenario==INVALID_EVENT ? POLLNVAL : (scenario==ERROR_EVENT ? POLLHUP : POLLOUT);
        ready++;
    }
    pthread_mutex_unlock(&fake_mutex);
    atomic_fetch_add(&fake_time, ready ? 1 : timeout);
    return ready;
}
#ifndef _WIN32
static int fake_fcntl(int fd, int command, ...) {
    (void)fd; (void)command;
    if (scenario==NONBLOCK_FAILURE) { errno=EIO; return -1; }
    return 0;
}
#else
static int fake_ioctl(SOCKET fd, long command, u_long *arg) {
    (void)fd; (void)command; (void)arg;
    if (scenario==NONBLOCK_FAILURE) { WSASetLastError(WSAEINVAL); return -1; }
    return 0;
}
#endif
static int fake_clock(clockid_t clock, struct timespec *value) {
    (void)clock;
    long long ms=atomic_load(&fake_time);
    value->tv_sec=(time_t)(ms/1000); value->tv_nsec=(long)(ms%1000)*1000000L;
    return 0;
}
static int fake_sleep(const struct timespec *a, struct timespec *b) { (void)a; (void)b; return 0; }
static int fake_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *p) {
    int n=atomic_fetch_add(&thread_creates,1);
    if (thread_failure_after>=0 && n>=thread_failure_after) return EAGAIN;
    return real_thread_create(t,a,fn,p);
}
static void setup(int kind) {
    scenario=kind; failures=0; active_handles=peak_handles=0; thread_failure_after=-1;
    memset(handles,0,sizeof(handles)); memset(attempts_seen,0,sizeof(attempts_seen));
    atomic_init(&fake_time,0); atomic_init(&thread_creates,0);
    atomic_init(&scan_cancel_requested,0); atomic_init(&ports_scanned,0);
    atomic_init(&host_responded,0); atomic_init(&latest_probe_port,0);
    portscanner_interrupt_requested=0;
    target_ip_global="192.0.2.10";
    results_list=calloc(SCAN_TOTAL_PORTS+1,sizeof(*results_list));
    CHECK(results_list); CHECK(pthread_mutex_init(&results_mutex,NULL)==0);
}
static void teardown(void) {
    CHECK(active_handles==0); CHECK(peak_handles<=scan_socket_budget());
    pthread_mutex_destroy(&results_mutex); free(results_list); results_list=NULL;
}
static void test_full_coverage(void) {
    for (int fault=-1;fault<=2;++fault) {
        setup(CLOSED); thread_failure_after=fault;
        run_scan_phase(NULL,SCAN_TOTAL_PORTS,16,512,50);
        scan_summary s=summarize_scan();
        CHECK(s.closed==65535 && s.unscanned==0);
        CHECK(atomic_load(&ports_scanned)==65535);
        CHECK(atomic_load(&host_responded)==1);
        for (int p=1;p<=65535;++p) CHECK(results_list[p].attempts==1 && attempts_seen[p]==1);
        teardown();
    }
    puts("PASS all closed: exact 1..65535 coverage, no duplicate workers, partial/all thread creation failure");
}
static void test_discovery_and_deep(void) {
    setup(MIXED);
    CHECK(host_probe_alive(target_ip_global)==HOST_UNKNOWN);
    /* One worker keeps the synthetic clock deterministic in timeout tests. */
    run_scan_phase(NULL,65535,1,256,10);
    CHECK(results_list[54321].state==PORT_OPEN);
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports);
    CHECK(n==65534);
    scenario=CLOSED;
    scan_profile profile={"test",1,256,10,1,64,20,37};
    run_deep_scan(ports,n,&profile);
    scan_summary s=summarize_scan();
    CHECK(s.open==1 && s.closed==65534 && s.filtered==0 && s.unknown==0 && s.unscanned==0);
    CHECK(s.retried==65534 && results_list[54321].attempts==1);
    CHECK(results_list[1].attempts==2 && results_list[65535].attempts==2);
    free(ports); teardown();
    puts("PASS unknown discovery does not gate full scan; all retry windows drained beyond old cutoff");
}
static void test_batch_outcomes(void) {
    const int ports[]={1,65535};
    const int cases[]={TIMEOUT,DELAYED,RESOURCE,POLL_FAILURE,GETSOCKOPT_FAILURE,INVALID_EVENT,
                       PENDING_EVENT,NONBLOCK_FAILURE,ROUTE_FAILURE,INTERRUPTED_POLL,ERROR_EVENT};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        setup(cases[i]);
        scan_ports_batch(target_ip_global,ports,2,250);
        port_state expected=PORT_UNKNOWN;
        if (cases[i]==TIMEOUT) expected=PORT_FILTERED;
        if (cases[i]==DELAYED || cases[i]==PENDING_EVENT || cases[i]==INTERRUPTED_POLL) expected=PORT_OPEN;
        CHECK(results_list[1].state==expected && results_list[65535].state==expected);
        CHECK(atomic_load(&ports_scanned)==2);
        teardown();
    }
    puts("PASS timeout, delayed connect, resource/nonblocking/poll/SO_ERROR/routing failures, invalid/HUP/pending events and EINTR");
}
static void test_full_timeouts(void) {
    setup(TIMEOUT);
    run_scan_phase(NULL,65535,1,256,5);
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports); CHECK(n==65535);
    scan_profile profile={"test",1,256,5,1,128,10,8000};
    run_deep_scan(ports,n,&profile);
    scan_summary s=summarize_scan();
    CHECK(s.filtered==65535 && s.closed==0 && s.unscanned==0 && s.retried==65535);
    CHECK(atomic_load(&ports_scanned)==65535 && atomic_load(&host_responded)==0);
    for (int p=1;p<=65535;++p) CHECK(results_list[p].attempts==2);
    free(ports); teardown();
    puts("PASS full filtered target: invariant holds, every candidate retried exactly once, host stays unknown");
}
static void test_cancel(void) {
    setup(CANCEL);
    run_scan_phase(NULL,65535,4,16,5000);
    scan_summary s=summarize_scan(); CHECK(scan_cancelled()); CHECK(s.unscanned==65535);
    CHECK(atomic_load(&ports_scanned)==0); teardown();
    setup(TIMEOUT);
    int ports[]={1,65535}; scan_ports_batch(target_ip_global,ports,2,1);
    scenario=CANCEL; scan_ports_batch(target_ip_global,ports,2,5000);
    CHECK(results_list[1].state==PORT_FILTERED && results_list[1].retries_completed==0);
    teardown();
    CHECK(portscanner(NULL)==PORTSCANNER_FAILED);
    CHECK(portscanner("192.0.2.999")==PORTSCANNER_FAILED);
    puts("PASS cancellation cleans active sockets and reports unscanned separately; invalid IPv4 rejected");
}
static void test_resource_recovery(void) {
    setup(RESOURCE);
    run_scan_phase(NULL,65535,1,256,5);
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports); CHECK(n==65535);
    scenario=CLOSED;
    scan_profile profile={"test",1,256,5,1,64,10,12000};
    run_deep_scan(ports,n,&profile);
    CHECK(summarize_scan().closed==65535 && summarize_scan().unknown==0);
    CHECK(summarize_scan().retried==65535);
    free(ports); teardown();
    puts("PASS every socket resource failure accounted as unknown, then retried to closed");
}
int main(void) {
#ifdef _WIN32
    _putenv_s("NERO_SCAN_ICMP", "");
#else
    unsetenv("NERO_SCAN_ICMP");
#endif
    test_full_coverage(); test_discovery_and_deep(); test_batch_outcomes();
    test_full_timeouts(); test_cancel(); test_resource_recovery();
    setup(CLOSED); teardown();
    CHECK(portscanner("192.0.2.10")==0);
    CHECK(atomic_load(&ports_scanned)==65535 && active_handles==0 && results_list==NULL);
    for (int p=1;p<=65535;++p) CHECK(attempts_seen[p]==1);
    puts("PASS production entry point: unknown discovery proceeds to all ports and cleans its ledger");
    puts("PASS scanner reliability: no real sockets or external targets contacted");
    return 0;
}
