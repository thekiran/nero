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
static int fake_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
#ifndef _WIN32
static int fake_fcntl(int, int, ...);
#else
static int fake_ioctl(SOCKET, long, u_long *);
#endif
/* Retain a non-macro path to the real pthread implementation. */
static int real_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    return pthread_mutex_init(m,a);
}
static int real_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *p) {
    return pthread_create(t, a, fn, p);
}
#ifndef _WIN32
/* Real descriptor close for the stdin pipes used by the cancel-key tests. */
static int real_close(int fd) { return close(fd); }
#endif
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
#define pthread_mutex_init fake_mutex_init
#ifndef _WIN32
#define fcntl fake_fcntl
#else
#define ioctlsocket fake_ioctl
#endif
#include "../src/modules/portscanner.c"
#undef pthread_create
#undef pthread_mutex_init

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
static int mutex_init_failure;
static int socket_cap;
enum { CLOSED, TIMEOUT, DELAYED, RESOURCE, MIXED, CANCEL, POLL_FAILURE,
       GETSOCKOPT_FAILURE, INVALID_EVENT, PENDING_EVENT, NONBLOCK_FAILURE, ROUTE_FAILURE,
       INTERRUPTED_POLL, ERROR_EVENT, TRANSIENT_RESOURCE, TRANSIENT_POLL, ALWAYS_INTERRUPTED, CAPPED };

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
    if (scenario==CAPPED && active_handles>=socket_cap) {
        pthread_mutex_unlock(&fake_mutex);
        errno=EMFILE; return NERO_INVALID_SOCKET; /* simulated descriptor ceiling */
    }
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
    const struct sockaddr_in *in=(const struct sockaddr_in *)addr;
    int port=ntohs(in->sin_port);
    CHECK(port>=1 && port<=SCAN_TOTAL_PORTS);
    /* Every synthetic connect must target the TEST-NET-1 documentation host. */
    CHECK(in->sin_family==AF_INET && ntohl(in->sin_addr.s_addr)==0xC000020Au);
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
    if (scenario==ALWAYS_INTERRUPTED) {
        atomic_fetch_add(&fake_time, 50);
        /* Deliberately poison revents: the failed poll must not classify open. */
        for (nfds_t s=0;s<count;++s) fds[s].revents=POLLOUT;
        errno=EINTR; return -1;
    }
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
static int fake_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    return mutex_init_failure ? EAGAIN : real_mutex_init(m,a);
}
static void setup(int kind) {
    scenario=kind; failures=0; active_handles=peak_handles=0; thread_failure_after=-1;
    memset(handles,0,sizeof(handles)); memset(attempts_seen,0,sizeof(attempts_seen));
    atomic_init(&fake_time,0); atomic_init(&thread_creates,0);
    atomic_init(&scan_cancel_requested,0); atomic_init(&ports_scanned,0);
    atomic_init(&host_responded,0); atomic_init(&latest_probe_port,0);
    atomic_init(&scan_progress_total,SCAN_TOTAL_PORTS);
    atomic_init(&probes_completed,0); atomic_init(&scan_work_done,0);
    atomic_init(&resource_failures,0); socket_cap=0;
    scan_input_fallback=0;
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
    scan_profile profile={"test",1,256,10,1,64,20,37,1};
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
                       PENDING_EVENT,NONBLOCK_FAILURE,ROUTE_FAILURE,INTERRUPTED_POLL,ERROR_EVENT,ALWAYS_INTERRUPTED};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        setup(cases[i]);
        scan_ports_batch(target_ip_global,ports,2,250);
        port_state expected=PORT_UNKNOWN;
        if (cases[i]==TIMEOUT || cases[i]==ALWAYS_INTERRUPTED) expected=PORT_FILTERED;
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
    scan_profile profile={"test",1,256,5,1,128,10,8000,3};
    run_deep_scan(ports,n,&profile);
    scan_summary s=summarize_scan();
    CHECK(s.filtered==65535 && s.closed==0 && s.unscanned==0 && s.retried==65535);
    CHECK(atomic_load(&ports_scanned)==65535 && atomic_load(&host_responded)==0);
    for (int p=1;p<=65535;++p) CHECK(results_list[p].attempts==2);
    free(ports); teardown();
    CHECK(s.retry_probes==65535);
    puts("PASS full filtered target: invariant holds, 3 rounds still retry each silent port exactly once, host stays unknown");
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
static void test_signal_interrupt(void) {
    setup(CLOSED);
    portscanner_interrupt_requested = 1;
    run_scan_phase(NULL,65535,4,64,50);
    scan_summary s=summarize_scan();
    CHECK(scan_cancelled() && s.unscanned==65535 && s.closed==0);
    CHECK(atomic_load(&ports_scanned)==0);
    portscanner_interrupt_requested = 0;
    atomic_store(&scan_cancel_requested,0);
    run_scan_phase(NULL,65535,4,64,50);
    CHECK(summarize_scan().closed==65535 && summarize_scan().unscanned==0);
    teardown();
    puts("PASS async interrupt stops work before attempts and a restart re-covers every port");
}
static void test_resource_recovery(void) {
    setup(RESOURCE);
    run_scan_phase(NULL,65535,1,256,5);
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports); CHECK(n==65535);
    scenario=CLOSED;
    scan_profile profile={"test",1,256,5,1,64,10,12000,1};
    run_deep_scan(ports,n,&profile);
    CHECK(summarize_scan().closed==65535 && summarize_scan().unknown==0);
    CHECK(summarize_scan().retried==65535);
    free(ports); teardown();
    puts("PASS every socket resource failure accounted as unknown, then retried to closed");
}
static void test_error_diagnostics(void) {
    setup(RESOURCE);
    run_scan_phase(NULL,65535,1,256,5);
    scan_error_group groups[SCAN_ERROR_GROUPS];
    int overflow = -1;
    int n = summarize_scan_errors(groups, &overflow);
    CHECK(n==1 && overflow==0 && groups[0].count==65535);
    CHECK(groups[0].error==EMFILE && groups[0].stage==SCAN_SOCKET);
    CHECK(nero_log_clear()==0);
    report_scan_summary();
    FILE *capture=tmpfile(); CHECK(capture);
    nero_log_dump(capture,1); rewind(capture);
    char log_output[4096]={0};
    CHECK(fread(log_output,1,sizeof(log_output)-1,capture)>0); fclose(capture);
    CHECK(strstr(log_output,"operation=socket")!=NULL);
    CHECK(strstr(log_output,"ports=65535")!=NULL);
    scenario=ROUTE_FAILURE;
    const int ports[]={1,65535};
    scan_ports_batch(target_ip_global,ports,2,5);
    n=summarize_scan_errors(groups,&overflow);
    CHECK(n==2 && overflow==0);
    CHECK(groups[0].error==EHOSTUNREACH && groups[0].stage==SCAN_CONNECT && groups[0].count==2);
    CHECK(groups[1].error==EMFILE && groups[1].count==65533);
    scenario=CLOSED;
    scan_ports_batch(target_ip_global,ports,2,5);
    n=summarize_scan_errors(groups,&overflow);
    CHECK(n==1 && groups[0].count==65533);
    char text[256]; scan_error_text(0,text,sizeof(text));
    CHECK(strstr(text,"not confirmed")!=NULL);
    scan_error_text(EMFILE,text,sizeof(text)); CHECK(text[0]!='\0');
    teardown();
    puts("PASS error diagnostics: operation/errno counts, retry replacement and unresolved readiness");
}
static void test_initialization_error_logs(void) {
    setup(CLOSED); teardown();
    CHECK(nero_log_clear()==0);
    mutex_init_failure=1;
    CHECK(portscanner("192.0.2.10")==PORTSCANNER_FAILED);
    CHECK(results_list==NULL && active_handles==0);
    mutex_init_failure=0;
    thread_failure_after=0; atomic_store(&thread_creates,0);
    int ports[]={443}; char store[1][BANNER_MAX]={{0}};
    grab_banners("192.0.2.10",ports,1,store);
    FILE *capture=tmpfile(); CHECK(capture);
    nero_log_dump(capture,1); rewind(capture);
    char output[4096]={0};
    CHECK(fread(output,1,sizeof(output)-1,capture)>0); fclose(capture);
    CHECK(strstr(output,"Scan mutex initialization failed")!=NULL);
    CHECK(strstr(output,"Banner worker creation failed")!=NULL);
    CHECK(active_handles==0);
    puts("PASS scanner mutex/banner initialization failures reach error logs");
}

static void test_port_bounds(void) {
    setup(CLOSED);
    const int ports[]={0,-1,65536,1,65535};
    scan_ports_batch(target_ip_global,ports,5,50);
    CHECK(results_list[1].state==PORT_CLOSED && results_list[65535].state==PORT_CLOSED);
    CHECK(results_list[0].state==PORT_UNSCANNED && results_list[0].attempts==0);
    CHECK(atomic_load(&ports_scanned)==2 && attempts_seen[1]==1 && attempts_seen[65535]==1);
    scan_summary s=summarize_scan();
    CHECK(s.closed==2 && s.unscanned==65533);
    CHECK(s.open+s.closed+s.filtered+s.unknown+s.unscanned==65535);
    teardown();
    puts("PASS first/last port probed; port 0 and out-of-range indices never enter the ledger");
}
typedef struct { int port; port_state state; } race_arg;
static void *race_recorder(void *p) {
    race_arg *a=p;
    for (int i=0;i<2000;++i)
        record_result(a->port,a->state,a->state==PORT_OPEN?0:EMFILE,
                      a->state==PORT_OPEN?SCAN_CONNECT:SCAN_SOCKET);
    return NULL;
}
static void test_retry_precedence(void) {
    setup(CLOSED);
    record_result(80,PORT_OPEN,0,SCAN_CONNECT);
    record_result(80,PORT_UNKNOWN,EMFILE,SCAN_SOCKET);
    record_result(80,PORT_FILTERED,ETIMEDOUT,SCAN_DEADLINE);
    CHECK(results_list[80].state==PORT_OPEN && results_list[80].error==0);
    CHECK(results_list[80].retries_completed==2);
    record_result(81,PORT_FILTERED,ETIMEDOUT,SCAN_DEADLINE);
    record_result(81,PORT_UNKNOWN,EMFILE,SCAN_SOCKET);
    CHECK(results_list[81].state==PORT_FILTERED && results_list[81].error==ETIMEDOUT);
    record_result(81,PORT_CLOSED,ECONNREFUSED,SCAN_CONNECT);
    CHECK(results_list[81].state==PORT_CLOSED && results_list[81].retries_completed==2);
    record_result(82,PORT_UNKNOWN,EMFILE,SCAN_SOCKET);
    record_result(82,PORT_UNKNOWN,EIO,SCAN_POLL);
    CHECK(results_list[82].state==PORT_UNKNOWN && results_list[82].error==EIO);
    CHECK(atomic_load(&ports_scanned)==3);
    /* Concurrent writers: an open result is never dropped by a failing one. */
    pthread_t t[8]; race_arg args[8];
    for (int i=0;i<8;++i) {
        args[i]=(race_arg){443,(i%2)?PORT_UNKNOWN:PORT_OPEN};
        CHECK(real_thread_create(&t[i],NULL,race_recorder,&args[i])==0);
    }
    for (int i=0;i<8;++i) CHECK(pthread_join(t[i],NULL)==0);
    CHECK(results_list[443].state==PORT_OPEN);
    CHECK(results_list[443].retries_completed==8*2000-1);
    CHECK(atomic_load(&ports_scanned)==4);
    teardown();
    puts("PASS retries never downgrade open/closed/filtered evidence, including under concurrent writers");
}
static void test_adaptive_retry(void) {
    /* A host-wide descriptor ceiling of 8 sockets: most of every first-pass
     * batch fails with EMFILE. Deep rounds must back off to the ceiling and
     * recover every port, without exceeding 1 + rounds probes per port. */
    setup(CAPPED); socket_cap=8;
    run_scan_phase(NULL,65535,1,256,10);
    scan_summary first=summarize_scan();
    CHECK(first.unknown>60000 && first.unscanned==0 && first.closed==65535-first.unknown);
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports); CHECK(n==first.unknown);
    scan_profile profile={"test",1,256,10,1,64,20,4096,3};
    run_deep_scan(ports,n,&profile);
    scan_summary s=summarize_scan();
    CHECK(s.closed==65535 && s.unknown==0 && s.unscanned==0);
    CHECK(peak_handles<=8);
    int third=0;
    for (int p=1;p<=65535;++p) {
        CHECK(results_list[p].attempts<=4);
        if (results_list[p].attempts==3) third++;
    }
    CHECK(third>0); /* a second round ran only for ports still unknown */
    free(ports); teardown();

    setup(RESOURCE);
    run_scan_phase(NULL,65535,1,256,5);
    ports=malloc(65535*sizeof(int)); CHECK(ports);
    n=collect_retry_candidates(ports); CHECK(n==65535);
    scan_profile bounded={"test",1,256,5,4,128,10,12000,3};
    run_deep_scan(ports,n,&bounded);
    s=summarize_scan();
    CHECK(s.unknown==65535 && s.closed==0 && s.retried==65535 && s.retry_probes==3*65535);
    for (int p=1;p<=65535;++p) CHECK(results_list[p].attempts==4);
    scan_profile excessive={"test",1,256,5,4,128,10,12000,99};
    for (int p=1;p<=65535;++p) ports[p-1]=p;
    run_deep_scan(ports,65535,&excessive);
    for (int p=1;p<=65535;++p) CHECK(results_list[p].attempts==4+MAX_DEEP_ROUNDS);
    free(ports); teardown();
    puts("PASS adaptive retry backs off to a descriptor ceiling, recovers every port, and rounds stay bounded");
}
static void test_progress_honest(void) {
    setup(TIMEOUT);
    CHECK(scan_progress_permille()==0);
    run_scan_phase(NULL,65535,1,256,5);
    CHECK(atomic_load(&probes_completed)==65535);
    CHECK(scan_progress_permille()==999); /* fast pass done is not scan done */
    int *ports=malloc(65535*sizeof(int)); CHECK(ports);
    int n=collect_retry_candidates(ports);
    atomic_fetch_add(&scan_progress_total,n);
    CHECK(scan_progress_permille()==500);
    atomic_fetch_sub(&scan_progress_total,n);
    scan_profile profile={"test",1,256,5,1,128,10,8000,1};
    run_deep_scan(ports,n,&profile);
    CHECK(atomic_load(&scan_progress_total)==2*65535);
    CHECK(scan_progress_permille()==999);
    atomic_store(&scan_work_done,1);
    CHECK(scan_progress_permille()==1000);
    free(ports); teardown();
    puts("PASS progress counts completed probes and reaches 100% only after the deep phase");
}
#ifndef _WIN32
static void with_stdin(const char *bytes, int keep_open, void (*body)(void)) {
    int saved=dup(STDIN_FILENO); CHECK(saved>=0);
    int fds[2]; CHECK(pipe(fds)==0);
    CHECK(write(fds[1],bytes,strlen(bytes))==(ssize_t)strlen(bytes));
    if (!keep_open) { CHECK(real_close(fds[1])==0); fds[1]=-1; }
    CHECK(dup2(fds[0],STDIN_FILENO)==STDIN_FILENO); CHECK(real_close(fds[0])==0);
    body();
    CHECK(dup2(saved,STDIN_FILENO)==STDIN_FILENO); CHECK(real_close(saved)==0);
    if (fds[1]>=0) CHECK(real_close(fds[1])==0);
}
static void menu_expect_cancel(void) {
    int ports[]={22,80};
    CHECK(select_port_menu(ports,2)==PORTSCANNER_CANCELLED);
}
static void scan_expect_esc_cancel(void) {
    setup(CLOSED);
    scan_owner=pthread_self(); scan_input_fallback=1;
    run_scan_phase(NULL,65535,1,64,50);
    scan_summary s=summarize_scan();
    CHECK(scan_cancelled() && s.unscanned==65535 && s.closed==0);
    teardown();
}
static void test_user_cancel_keys(void) {
    with_stdin("\033",1,menu_expect_cancel);   /* standalone Esc */
    with_stdin("\003",1,menu_expect_cancel);   /* Ctrl+C byte in raw mode */
    portscanner_interrupt_requested=1;         /* SIGINT handler path */
    with_stdin("",1,menu_expect_cancel);
    portscanner_interrupt_requested=0;
    with_stdin("\033",1,scan_expect_esc_cancel);
    puts("PASS Esc/Ctrl+C/SIGINT cancel the port menu, and Esc stops queued scan work as unscanned");
}
#endif
int main(void) {
#ifdef _WIN32
    _putenv_s("NERO_SCAN_ICMP", "");
#else
    unsetenv("NERO_SCAN_ICMP");
#endif
    test_initialization_error_logs();
    test_error_diagnostics();
    test_full_coverage(); test_discovery_and_deep(); test_batch_outcomes();
    test_full_timeouts(); test_cancel(); test_signal_interrupt(); test_resource_recovery();
    test_port_bounds(); test_retry_precedence(); test_adaptive_retry(); test_progress_honest();
#ifndef _WIN32
    test_user_cancel_keys();
#endif
    setup(CLOSED); teardown();
    CHECK(portscanner("192.0.2.10")==0);
    CHECK(atomic_load(&ports_scanned)==65535 && active_handles==0 && results_list==NULL);
    for (int p=1;p<=65535;++p) CHECK(attempts_seen[p]==1);
    puts("PASS production entry point: unknown discovery proceeds to all ports and cleans its ledger");
    puts("PASS scanner reliability: no real sockets or external targets contacted");
    return 0;
}
