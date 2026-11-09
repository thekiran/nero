#ifndef NERO_SCAN_SESSION_H
#define NERO_SCAN_SESSION_H
#define NERO_SCAN_PORT_COUNT 65535
typedef struct nero_scan_session nero_scan_session;
typedef enum {
    NERO_PORT_UNSCANNED = 0,
    NERO_PORT_OPEN,
    NERO_PORT_CLOSED,
    NERO_PORT_FILTERED,
    NERO_PORT_UNKNOWN
} nero_port_state;
typedef enum {
    NERO_SCAN_FAILED = -1,
    NERO_SCAN_COMPLETE = 0,
    NERO_SCAN_CANCELLED,
    NERO_SCAN_PARTIAL
} nero_scan_status;
typedef struct {
    const char *name;
    int fast_threads, fast_batch, fast_timeout_ms;
    int deep_threads, deep_batch, deep_timeout_ms;
    int deep_max_candidates, deep_rounds;
} nero_scan_profile;
typedef struct {
    int open, closed, filtered, unknown, unscanned;
    int retried, retry_probes;
} nero_scan_counts;
typedef struct {
    nero_port_state state;
    int error;
    unsigned attempts, retries_completed;
} nero_scan_result;
/* Numeric IPv4. NULL profile uses balanced defaults. Configuration is copied.
 * Core performs no terminal I/O, discovery, banners or signal installation.
 * Caller initializes Winsock on Windows. Errors set errno. */
nero_scan_session *nero_scan_create(const char *, const nero_scan_profile *);
/* One-shot synchronous run. COMPLETE means coverage/retries finished, not that
 * every result is certain. Other threads may cancel/stop/read snapshots.
 * Repeated or simultaneous run on the same session fails with EALREADY/EBUSY. */
nero_scan_status nero_scan_run(nero_scan_session *);
void nero_scan_cancel(nero_scan_session *);
void nero_scan_stop_early(nero_scan_session *);
int nero_scan_snapshot(nero_scan_session *, nero_scan_counts *);
int nero_scan_get_result(nero_scan_session *, int port, nero_scan_result *);
/* Destroy only after run and all concurrent callers return. NULL is safe. */
void nero_scan_destroy(nero_scan_session *);
#endif
