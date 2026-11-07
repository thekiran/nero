#ifndef NERO_LOG_H
#define NERO_LOG_H
#include <stdio.h>

typedef enum { NERO_LOG_INFO, NERO_LOG_WARN, NERO_LOG_ERROR } nero_log_level;
/* Thread-safe in normal execution; never call from a signal handler.
 * Lazy initialization, bounded memory/file history, and errno preservation.
 * NERO_LOG_FILE overrides the per-user path; :memory: disables disk storage. */
void nero_log_write(nero_log_level level, const char *component, const char *format, ...);
void nero_log_dump(FILE *out, int errors_only);
void nero_log_show_path(FILE *out);
int nero_log_clear(void);
#endif
