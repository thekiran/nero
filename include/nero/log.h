#ifndef NERO_LOG_H
#define NERO_LOG_H
#include <stdio.h>

/* Severity levels, ascending. TRACE/DEBUG are diagnostic; INFO is the default
 * runtime floor; WARN flags recoverable trouble; ERROR/FATAL are never
 * suppressed by the runtime level filter. The persisted record stores the
 * level *name*, so these numeric values are an internal detail and may be
 * reordered without affecting existing log files. */
typedef enum {
    NERO_LOG_TRACE, NERO_LOG_DEBUG, NERO_LOG_INFO,
    NERO_LOG_WARN, NERO_LOG_ERROR, NERO_LOG_FATAL
} nero_log_level;

/* Core writer. Thread-safe in normal execution; never call from a signal
 * handler. Lazy initialization, bounded memory/file history, and errno
 * preservation. Values of sensitive keys (password/passwd/pwd/secret/token/
 * apikey/api_key/key/cookie/authorization/bearer/access_token/refresh_token/
 * private_key) are redacted before the record is stored. Records below the
 * runtime level (see nero_log_set_level) are dropped cheaply, except ERROR and
 * FATAL which are always emitted. NERO_LOG_FILE overrides the per-user path;
 * :memory: disables disk storage. */
void nero_log_write(nero_log_level level, const char *component, const char *format, ...);
void nero_log_dump(FILE *out, int errors_only);
void nero_log_show_path(FILE *out);
int nero_log_clear(void);

/* Runtime minimum level. Session-scoped (never persisted); defaults to INFO.
 * A value above NERO_LOG_ERROR is clamped to NERO_LOG_ERROR so errors are
 * never silenced. get returns the active floor; from_name parses a case-
 * insensitive level name (returns 0 and sets *out on success, -1 on an unknown
 * name); name returns the canonical upper-case label. */
void nero_log_set_level(nero_log_level level);
nero_log_level nero_log_get_level(void);
int nero_log_level_from_name(const char *name, nero_log_level *out);
const char *nero_log_level_name(nero_log_level level);

/* Stable per-process identifier shared by every record, for correlation. */
const char *nero_log_session_id(void);

/* Filtered listing of the in-memory ring (bounded to <= 256 records). Any
 * field left zero/NULL is disabled. min_level drops records below it (use
 * NERO_LOG_TRACE to keep all); component matches the [component] field
 * exactly; substring is a case-sensitive "contains" over the whole record;
 * tail > 0 keeps only the last `tail` matching records. */
typedef struct {
    nero_log_level min_level;
    int tail;
    const char *component;
    const char *substring;
} nero_log_query;
void nero_log_dump_query(FILE *out, const nero_log_query *query);

/* JSON Lines rendering of the same records (one object per line, pinned
 * schema_version). JSON is an output rendering only: the on-disk format stays
 * text, so reading historical records is never affected. */
void nero_log_export_json(FILE *out, const nero_log_query *query);

/* Operational counters and buffer/storage status (writes, persist failures,
 * ring evictions, rotations, redactions, buffer usage, last storage error). */
void nero_log_health(FILE *out);

/* Aggregate view of the in-memory ring: record counts per level and the most
 * active components. */
void nero_log_stats(FILE *out);
#endif
