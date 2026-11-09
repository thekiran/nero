#ifndef NERO_SCAN_INTERNAL_H
#define NERO_SCAN_INTERNAL_H
#ifdef _WIN32
#include "../platform/windows/nero_win_compat.h"
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include "nero/scan_session.h"
#include "nero/socket_handle.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#define SCAN_TOTAL_PORTS 65535
#define DISCOVERY_VIEW_CAP 512

/* Yığın dizilerinin derleme-zamanı üst sınırları. Tüm profiller ve manuel
 * ayar bu sınırların altında tutulur (poll() kullandığımız için FD_SETSIZE
 * sorunu yoktur; sınırlar yalnızca bellek ve hedefe nezaket içindir). */
#define MAX_THREADS 16
#define MAX_BATCH 512
#define MAX_SCAN_SOCKETS 256
#define CANCEL_POLL_MS 50
#define MAX_DEEP_ROUNDS 4
#define MAX_DEEP_TIMEOUT_MS 5000

#define SCAN_ERROR_GROUPS 16
#define PORT_UNSCANNED NERO_PORT_UNSCANNED
#define PORT_OPEN NERO_PORT_OPEN
#define PORT_CLOSED NERO_PORT_CLOSED
#define PORT_FILTERED NERO_PORT_FILTERED
#define PORT_UNKNOWN NERO_PORT_UNKNOWN
typedef nero_port_state port_state;
typedef nero_scan_profile scan_profile;
enum { PROFILE_FAST = 0, PROFILE_MEDIUM, PROFILE_SLOW, PROFILE_MANUAL, PROFILE_COUNT };
extern const scan_profile SCAN_PROFILES[3];
typedef enum {
    SCAN_SOCKET,
    SCAN_NONBLOCKING,
    SCAN_CONNECT,
    SCAN_POLL,
    SCAN_SO_ERROR,
    SCAN_POLL_EVENT,
    SCAN_DEADLINE
} scan_error_stage;

typedef struct {
    port_state state;
    int error;
    scan_error_stage error_stage;
    unsigned attempts;
    unsigned retries_completed;
} port_result;

typedef struct {
    nero_scan_session *session;
    atomic_int next;
    const int *ports; /* NULL means the complete 1..65535 range. */
    int count, timeout_ms, batch;
} scan_work;

typedef nero_scan_counts scan_summary;

typedef struct {
    scan_error_stage stage;
    int error, count;
} scan_error_group;
/* Private per-session dependency injection. NULL members keep native I/O.
 * Callbacks obey native errno contracts; no production-global test switches. */
typedef struct {
    nero_socket_t (*socket_fn)(int, int, int);
    int (*connect_fn)(nero_socket_t, const struct sockaddr *, socklen_t);
    int (*getsockopt_fn)(nero_socket_t, int, int, void *, socklen_t *);
    int (*close_fn)(nero_socket_t);
    int (*poll_fn)(struct pollfd *, nfds_t, int);
    int (*nonblocking_fn)(nero_socket_t);
    int (*clock_fn)(clockid_t, struct timespec *);
    int (*sleep_fn)(const struct timespec *, struct timespec *);
    int (*thread_create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    int (*mutex_init_fn)(pthread_mutex_t *, const pthread_mutexattr_t *);
} scan_io;
struct nero_scan_session {
    char target[16], profile_name[32];
    const char *target_ip_global;
    scan_profile profile;
    port_result *results_list;
    int *work_ports;
    int retry_candidates;
    pthread_mutex_t results_mutex;
    scan_io io;
    atomic_int scan_cancel_requested;
    atomic_int ports_scanned;
    atomic_int latest_probe_port;
    atomic_int host_responded;
    atomic_int scan_progress_total;
    atomic_int probes_completed;
    atomic_int scan_work_done;
    atomic_int scan_select_requested;
    atomic_int resource_failures;
    atomic_int worker_failures;
    atomic_int fatal_error;
    atomic_int lifecycle;

    const volatile sig_atomic_t *interrupt_requested;
    int (*check_cancel)(void *);
    void *cancel_context;
    void (*on_open)(void *);
    void *event_context;
    char scan_operation_id[40];
    const char *scan_profile_label, *scan_completion;
    int64_t scan_duration_ms;
};
nero_scan_session *scan_create_with_io(const char *, const scan_profile *, const scan_io *);
int scan_cancelled(nero_scan_session *s);
int scan_interrupted(nero_scan_session *s);
int64_t scan_now_ms(nero_scan_session *s);
scan_summary summarize_scan(nero_scan_session *s);
void record_result(nero_scan_session *s, int port, port_state state, int error,
                   scan_error_stage stage);
int scan_progress_permille(nero_scan_session *s);
int summarize_scan_errors(nero_scan_session *s, scan_error_group *groups, int *overflow);
int scan_socket_budget(void);
int connect_pending(int error);
int make_nonblocking(nero_socket_t fd);
void scan_ports_batch(nero_scan_session *s, const char *ip, const int *ports, int count,
                      int timeout_ms);
void run_scan_phase(nero_scan_session *s, const int *ports, int count, int threads, int batch,
                    int timeout_ms);
int collect_retry_candidates(nero_scan_session *s, int *ports);
void run_deep_scan(nero_scan_session *s, int *ports, int count, const scan_profile *profile);
#endif
